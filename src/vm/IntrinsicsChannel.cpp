/*---
    IntrinsicsChannel.cpp — K1 Slice 9 / K3 Task 4 Channel<T> intrinsic
    family (138-141): construction (the capacity contract), send/recv/
    close, and the side-table slots. Split out of IntrinsicsThread.cpp
    (the 500-line source budget).
    K8: every arm runs under the Sync domain — structural sections
    (slot resolution, table mutation, waiter registration) stay inside
    the domain while park/wake scheduler calls run between sections
    (a domain lock is never held across a scheduler call). Parking
    rides the two-phase handshake: Sync{register} → scheduler{ParkFor*}
    → Sync{recheck}. The recheck commits on a still-kPending entry
    (RequestPark; a waker in the mark→steal stretch delivers through
    the K7 framesParked handshake) and aborts on a stamp — the outcome
    self-serves at the call site (a value already written through
    resultPtr, or the closed exception raised after the release). A
    waker inside the registration→migration window stamps the entry
    and wakes a Runnable-held context — a no-op — relying on the
    recheck to abort instead of park: the lost-wakeup window closed.
    Stamps stay in Sync-owned table state — never a scheduler flag,
    which would race a mid-park abort's unlocked read.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Slice 9: Channel() construction — materialize the side-table slot and
//write the handle into __handle (the Mutex shape: the receiver is the
//instance record at callParamBase slot 0). Phase-2: the two declared
//ctor overloads share this tail; the zero-arg intrinsic passes the
//unbounded constant and never reads the unstaged slot 1.
bool VmExecutor::IntrinsicChannelCtorAlloc(int32_t thisIdx, int32_t capacity)
{
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Channel constructor");
    if (capacity < kChannelUnboundedCapacity)
        RaiseNlangException(m_exceptionClassIdx,
            "channel capacity must be 0 (unbounded) or at least 1");
    //K8: the capacity write joins the allocation inside the allocator's
    //Sync section — the minted handle is not yet visible, but a
    //concurrent growth reallocates the store vector, so the slot write
    //shares the structure section.
    const int32_t handle = AllocChannelHandle(capacity);
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = handle;
    return true;
}

//The declared `Channel(int capacity)` overload: slot 1 = capacity
//(0 = unbounded); negative raises the catchable capacity error.
bool VmExecutor::IntrinsicChannelCtor(uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    int32_t capacity;
    std::memcpy(&capacity, locals + callParamBase + kFrameSlotBytes,
                sizeof(capacity));
    return IntrinsicChannelCtorAlloc(thisIdx, capacity);
}

//The declared `Channel()` overload (capacity 0 = unbounded). A zero-arg
//call stages nothing into slot 1 — the scratch there holds the previous
//eval's garbage, so the capacity must be the constant here.
bool VmExecutor::IntrinsicChannelCtorUnbounded(uint16_t callParamBase,
                                               uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    return IntrinsicChannelCtorAlloc(thisIdx, kChannelUnboundedCapacity);
}

//send(v): closed → the catchable ChannelClosedException (deferred — a
//reverse acquisition under the domain); a live waiter → write the value
//through its saved destination (the buffer is empty — invariant) and
//wake after the release (committed) or stamp kValue (pending — the
//aborting parker self-serves); bounded and full → the two-phase park
//(value+owner FIFO; recv's slot-free path stages the value into the
//buffer). Values are erased heap handles.
bool VmExecutor::IntrinsicChannelSend(uint16_t callParamBase, uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Channel call");
    int32_t value;
    std::memcpy(&value, locals + callParamBase + kFrameSlotBytes,
                sizeof(value));
    const int selfId = m_spScheduler->CurrentContextId();
    int wakeRecv = -1;
    bool mustPark = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        ChannelSlot* slot = SyncChannelSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Channel call on an uninitialized instance");
        if (slot->closed)
            throw DeferredNlangRaise{m_channelClosedExcClassIdx,
                                     "send on a closed channel"};
        if (!HandoffToLiveWaiter(*slot, value, wakeRecv)) {
            if (slot->capacity > 0
                && slot->buffered.size()
                       >= static_cast<size_t>(slot->capacity)) {
                //Gate BEFORE mutating scheduler state (decision 2 spec
                //(a)): the nested-window rejection must not leave a
                //half-parked context.
                EnsureSuspensionAllowed();
                slot->pending.push_back(PendingSend{value, selfId,
                    PendingSend::Phase::kPending});
                mustPark = true;
            } else {
                slot->buffered.push_back(value);
            }
        }
    }
    if (wakeRecv >= 0) {
        m_spScheduler->WakeForChanRecv(wakeRecv);
        return true;
    }
    if (!mustPark)
        return true;
    m_spScheduler->ParkForChanSend(selfId);
    CommitOrAbortChanSend(handle, selfId);
    return true;   //kValue: recv staged the value into the buffer
}

//recv(): buffer non-empty → pop the head into pResult (the freed slot
//stages the head live pending value and wakes its owner after the
//release); closed and drained → the catchable ChannelClosedException
//(deferred); empty and open → the two-phase park (the destination
//pointer pResult is registered — a parked call is not re-executed, the
//pc-advance invariant; send's handoff or close's delivery/exception
//write through it).
bool VmExecutor::IntrinsicChannelRecv(uint16_t callParamBase, uint8_t* locals,
                                      uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Channel call");
    const int selfId = m_spScheduler->CurrentContextId();
    int wakeSender = -1;
    bool mustPark = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        ChannelSlot* slot = SyncChannelSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Channel call on an uninitialized instance");
        if (!slot->buffered.empty()) {
            const int32_t value = slot->buffered.front();
            slot->buffered.pop_front();
            wakeSender = StagePendingSend(*slot);
            std::memcpy(pResult, &value, sizeof(value));
        } else if (slot->closed) {
            throw DeferredNlangRaise{m_channelClosedExcClassIdx,
                "recv on a closed and drained channel"};
        } else {
            //Gate BEFORE mutating scheduler state (decision 2 spec (a)):
            //the nested-window rejection must not leave a half-parked
            //context.
            EnsureSuspensionAllowed();
            slot->waiters.push_back(ChannelWaiter{selfId, pResult,
                ChannelWaiter::Phase::kPending});
            mustPark = true;
        }
    }
    if (wakeSender >= 0)
        m_spScheduler->WakeForChanSend(wakeSender);
    if (!mustPark)
        return true;
    m_spScheduler->ParkForChanRecv(selfId);
    if (CommitOrAbortChanRecv(handle, selfId) == AbortOutcome::kClosed)
        RaiseNlangException(m_channelClosedExcClassIdx,
            "recv on a closed and drained channel");   //[[noreturn]]
    return true;   //kValue: the deliverer already wrote pResult
}

//close(): idempotent flag; waiters drain buffered values first (sent
//values precede the close signal — K3 Task 3), the rest get the
//catchable ChannelClosedException; parked senders' pending values are
//dropped. K8: committed entries deliver through resultPtr / arm via
//the wake call's rethrow parameter; pending entries take stamps their
//aborting parkers self-serve. The exception OBJECTS build after the
//release (a Container acquisition is reverse under Sync) and arm
//inside the wake calls' scheduler critical sections.
bool VmExecutor::IntrinsicChannelClose(uint16_t callParamBase, uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Channel call");
    std::vector<int> valueWoken;    //committed waiters that drained a value
    std::vector<int> closedRecvs;
    std::vector<int> closedSenders;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        ChannelSlot* slot = SyncChannelSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Channel call on an uninitialized instance");
        if (slot->closed)
            return true;
        slot->closed = true;
        CollectClosedRecvOutcomes(*slot, valueWoken, closedRecvs);
        CollectClosedSenders(*slot, closedSenders);
    }
    WakeClosedWaiters(closedRecvs, closedSenders);
    for (int id : valueWoken)
        m_spScheduler->WakeForChanRecv(id);
    return true;
}

//K8: close's waiter sweep — committed entries deliver through resultPtr
//(value) or join the closed audience (armed after the release); pending
//entries take stamps their aborting parkers self-serve. Buffered values
//drain first (sent values precede the close signal — K3 Task 3).
void VmExecutor::CollectClosedRecvOutcomes(ChannelSlot& slot,
                                           std::vector<int>& valueWoken,
                                           std::vector<int>& closedRecvs)
{
    for (auto it = slot.waiters.begin(); it != slot.waiters.end();) {
        if (it->phase == ChannelWaiter::Phase::kCommitted) {
            if (!slot.buffered.empty()) {
                std::memcpy(it->resultPtr, &slot.buffered.front(),
                            sizeof(int32_t));
                slot.buffered.pop_front();
                valueWoken.push_back(it->contextId);
            } else {
                closedRecvs.push_back(it->contextId);
            }
            it = slot.waiters.erase(it);
        } else if (it->phase == ChannelWaiter::Phase::kPending) {
            if (!slot.buffered.empty()) {
                std::memcpy(it->resultPtr, &slot.buffered.front(),
                            sizeof(int32_t));
                slot.buffered.pop_front();
                it->phase = ChannelWaiter::Phase::kValue;
            } else {
                it->phase = ChannelWaiter::Phase::kClosed;
            }
            ++it;   //stamped entries stay for their aborting parkers
        } else {
            ++it;   //dead mailbox of an in-flight abort
        }
    }
}

//K8: close's sender sweep — committed senders join the closed audience
//(armed after the release); pending senders get kClosed stamps. Pending
//values are dropped (never delivered — the channel is closed).
void VmExecutor::CollectClosedSenders(ChannelSlot& slot,
                                      std::vector<int>& closedSenders)
{
    for (auto it = slot.pending.begin(); it != slot.pending.end();) {
        if (it->phase == PendingSend::Phase::kCommitted) {
            closedSenders.push_back(it->senderId);
            it = slot.pending.erase(it);
        } else {
            if (it->phase == PendingSend::Phase::kPending)
                it->phase = PendingSend::Phase::kClosed;
            ++it;
        }
    }
}

//K8: the recheck half of the two-phase handshake for a parked recv.
//Commit path: mark the entry kCommitted, then RequestPark throws out
//of the guard — a sender/close in the mark→steal stretch delivers
//through resultPtr and the K7 framesParked handshake enqueues us.
//Abort path: pop the entry, undo the state migration (the frames never
//left this carrier), report the stamp.
VmExecutor::AbortOutcome VmExecutor::CommitOrAbortChanRecv(int32_t handle,
                                                           int selfId)
{
    AbortOutcome outcome = AbortOutcome::kValue;
    bool commit = false;
    bool found = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        ChannelSlot* slot = SyncChannelSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Channel call on an uninitialized instance");
        for (auto it = slot->waiters.begin(); it != slot->waiters.end();
             ++it) {
            if (it->contextId != selfId)
                continue;
            found = true;
            if (it->phase == ChannelWaiter::Phase::kPending) {
                it->phase = ChannelWaiter::Phase::kCommitted;
                commit = true;
            } else {
                if (it->phase == ChannelWaiter::Phase::kClosed)
                    outcome = AbortOutcome::kClosed;
                slot->waiters.erase(it);   //consume the stamp
            }
            break;
        }
        if (!found)
            throw std::runtime_error(
                "NLang VM: channel waiter entry vanished");
    }
    if (commit)
        RequestPark(ParkSignal::Reason::ChanRecv);   //[[noreturn]]
    m_spScheduler->AbortPark(selfId);
    return outcome;
}

//K8: the recheck half for a parked send (same shape; the outcome raises
//at the call site — kValue means recv staged the value into the
//buffer, the send is complete).
void VmExecutor::CommitOrAbortChanSend(int32_t handle, int selfId)
{
    bool commit = false;
    bool closed = false;
    bool found = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        ChannelSlot* slot = SyncChannelSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Channel call on an uninitialized instance");
        for (auto it = slot->pending.begin(); it != slot->pending.end();
             ++it) {
            if (it->senderId != selfId)
                continue;
            found = true;
            if (it->phase == PendingSend::Phase::kPending) {
                it->phase = PendingSend::Phase::kCommitted;
                commit = true;
            } else {
                closed = it->phase == PendingSend::Phase::kClosed;
                slot->pending.erase(it);   //consume the stamp
            }
            break;
        }
        if (!found)
            throw std::runtime_error(
                "NLang VM: channel sender entry vanished");
    }
    if (commit)
        RequestPark(ParkSignal::Reason::ChanSend);   //[[noreturn]]
    m_spScheduler->AbortPark(selfId);
    if (closed)
        RaiseNlangException(m_channelClosedExcClassIdx,
                            "send on a closed channel");   //[[noreturn]]
}

//K8: send's handoff — the first live waiter (kPending or kCommitted;
//stamped entries are dead mailboxes their aborting parkers pop).
//Writes the value through its saved destination, then either pops the
//committed entry (reporting its context for the post-release wake) or
//stamps kPending kValue (the aborting parker self-serves). Returns
//false when nobody waits (send buffers or parks instead).
bool VmExecutor::HandoffToLiveWaiter(ChannelSlot& slot, int32_t value,
                                     int& wakeRecv)
{
    for (auto it = slot.waiters.begin(); it != slot.waiters.end(); ++it) {
        if (it->phase != ChannelWaiter::Phase::kPending
            && it->phase != ChannelWaiter::Phase::kCommitted)
            continue;
        std::memcpy(it->resultPtr, &value, sizeof(value));
        if (it->phase == ChannelWaiter::Phase::kCommitted) {
            wakeRecv = it->contextId;
            slot.waiters.erase(it);
        } else {
            it->phase = ChannelWaiter::Phase::kValue;
        }
        return true;
    }
    return false;
}

//K8: a buffer slot freed — stage the head live pending value into the
//buffer (FIFO; dead stamps skipped). A committed sender wakes (resume
//only — the parked call is not re-executed, its value is queued); a
//pending sender gets stamped kValue (its aborting recheck returns with
//the send complete). Returns the sender to wake after the release, -1
// when none.
int VmExecutor::StagePendingSend(ChannelSlot& slot)
{
    for (auto it = slot.pending.begin(); it != slot.pending.end(); ++it) {
        if (it->phase == PendingSend::Phase::kPending) {
            slot.buffered.push_back(it->value);
            it->phase = PendingSend::Phase::kValue;
            return -1;
        }
        if (it->phase == PendingSend::Phase::kCommitted) {
            slot.buffered.push_back(it->value);
            const int sender = it->senderId;
            slot.pending.erase(it);
            return sender;
        }
    }
    return -1;
}

//K8: close's exception arms — one prebuilt object per audience, armed
//inside the wake call's scheduler critical section (a committed
//context consumes the flag on resume; stamped parkers never see it —
//they self-serve their stamps).
void VmExecutor::WakeClosedWaiters(const std::vector<int>& recvs,
                                   const std::vector<int>& senders)
{
    int32_t recvExc = 0;
    for (int id : recvs) {
        if (recvExc == 0)
            recvExc = BuildNlangException(m_channelClosedExcClassIdx,
                "recv on a closed and drained channel");
        m_spScheduler->WakeForChanRecv(id, recvExc);
    }
    int32_t sendExc = 0;
    for (int id : senders) {
        if (sendExc == 0)
            sendExc = BuildNlangException(m_channelClosedExcClassIdx,
                "send on a closed channel");
        m_spScheduler->WakeForChanSend(id, sendExc);
    }
}

//K8 shared with the Mutex family (IntrinsicsThread.cpp): side-table
//lookup under the caller's Sync guard; null defers the
//uninitialized-instance error.
VmExecutor::ChannelSlot* VmExecutor::SyncChannelSlotAt(int32_t handle) {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_channelStore.size())
        return nullptr;
    return &m_channelStore[static_cast<size_t>(handle) - 1];
}

//Slice 9: allocate a channel side-table slot (handle = index+1, 0
//reserved for null). Reuse resets the whole slot — the sweep only
//returns handles; buffered/waiters/pending may carry a discarded
//channel's state (same discipline as the mutex). K8: the allocator
//owns the Sync-domain guard (reentrant for already-inside callers);
//the capacity write shares the allocation section (a concurrent growth
//reallocates the store vector).
int32_t VmExecutor::AllocChannelHandle(int32_t capacity) {
    DomainGuard g(m_domainLocks, DomainId::Sync);
    if (!m_channelFreeList.empty()) {
        const int32_t handle = m_channelFreeList.back();
        m_channelFreeList.pop_back();
        m_channelStore[static_cast<size_t>(handle) - 1] = ChannelSlot();
        m_channelStore[static_cast<size_t>(handle) - 1].capacity = capacity;
        return handle;
    }
    m_channelStore.emplace_back();
    m_channelStore.back().capacity = capacity;
    return static_cast<int32_t>(m_channelStore.size());
}

} // namespace nlang
