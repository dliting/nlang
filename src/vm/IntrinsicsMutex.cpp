/*---
    IntrinsicsMutex.cpp — K1 Slice 8 / K8: the Mutex intrinsic family
    (ctor, lock/tryLock/unlock) and the shared sync-primitive side-table
    helpers. Split out of IntrinsicsThread.cpp (the 500-line source
    budget). K8: the arms run under the Sync domain with the two-phase
    park handshake (see IntrinsicsChannel.cpp for the shared design
    note); raises inside a section defer (DeferredNlangRaise) or pass
    through as plain runtime_errors, and scheduler calls run between
    the sections — never under the domain.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//Slice 8: Mutex() 构造——物化侧表槽位并把句柄写进 __handle（同 List
//构造形态：接收者即 callParamBase 槽 0 的实例记录）。
bool VmExecutor::IntrinsicMutexCtor(uint16_t callParamBase, uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Mutex constructor");
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = AllocMutexHandle();
    return true;
}

//lock(): free means take it (ownerId = the current context); held
//(including self-held — non-reentrant) parks FIFO, unlock hands off.
//K8 two-phase park handshake: register under Sync, migrate the state
//with the scheduler, recheck under Sync — our entry still kPending
//means no unlock consumed it: commit the park. Stamped kAcquired means
//an unlock already transferred ownership to us inside the
//registration→migration window: abort the migration in place (the
//frames never left this carrier) and return holding the lock. The
//recheck closes the lost-wakeup window: an unlock inside that stretch
//stamps the entry and wakes a Runnable-held context — a no-op —
//relying on this recheck to abort instead of park.
bool VmExecutor::IntrinsicMutexLock(uint16_t callParamBase, uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Mutex call");
    const int selfId = m_spScheduler->CurrentContextId();
    bool mustPark = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        MutexSlot* slot = SyncMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Mutex call on an uninitialized instance");
        if (slot->ownerId < 0) {
            slot->ownerId = selfId;
        } else {
            //Gate BEFORE mutating scheduler state (decision 2 spec (a)):
            //the nested-window rejection must not leave a half-parked
            //context.
            EnsureSuspensionAllowed();
            slot->waiters.push_back(
                SyncWaiter{selfId, SyncWaiter::Phase::kPending});
            mustPark = true;
        }
    }
    if (!mustPark)
        return true;
    m_spScheduler->ParkForMutex(selfId);
    ConsumeMutexParkStamp(handle, selfId);   //commits or consumes a stamp
    m_spScheduler->AbortPark(selfId);        //stamp path: never stole
    return true;
}

//K8: the recheck half of the mutex handshake — delegates to the shared
//stamp consumer (P2-1) with the mutex park reason.
void VmExecutor::ConsumeMutexParkStamp(int32_t handle, int selfId)
{
    DomainGuard g(m_domainLocks, DomainId::Sync);
    MutexSlot* slot = SyncMutexSlotAt(handle);
    if (!slot)
        throw std::runtime_error(
            "NLang VM: Mutex call on an uninitialized instance");
    ConsumeSyncWaiterStamp(slot->waiters, selfId,
                           ParkSignal::Reason::Mutex);
}

//P2-1: the stamp-consumption core shared by the Mutex/Semaphore recheck
//halves — a still-kPending entry commits (kCommitted + RequestPark,
//[[noreturn]]; a waker in the mark→steal stretch delivers through the
//K7 framesParked handshake), a kAcquired stamp is consumed here and
//the caller aborts the migration. A missing entry is a corrupt-table
//tripwire.
void VmExecutor::ConsumeSyncWaiterStamp(std::deque<SyncWaiter>& waiters,
                                        int selfId,
                                        ParkSignal::Reason reason)
{
    for (auto it = waiters.begin(); it != waiters.end(); ++it) {
        if (it->contextId != selfId)
            continue;
        if (it->phase == SyncWaiter::Phase::kPending) {
            it->phase = SyncWaiter::Phase::kCommitted;
            RequestPark(reason);   //[[noreturn]]
        }
        waiters.erase(it);   //consume the stamp
        return;
    }
    throw std::runtime_error("NLang VM: sync waiter entry vanished");
}

//tryLock()：空闲取之返 true，被持返 false——绝不停泊（唯一不挂起的探
//测路径，可在嵌套求值窗口内安全调用）。
bool VmExecutor::IntrinsicMutexTryLock(uint16_t callParamBase,
                                       uint8_t* locals, uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Mutex call");
    int32_t r = 0;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        MutexSlot* slot = SyncMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Mutex call on an uninitialized instance");
        if (slot->ownerId < 0) {
            slot->ownerId = m_spScheduler->CurrentContextId();
            r = 1;
        }
    }
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//unlock(): not the owner → basic error; no live waiter → free; else
//hand ownership to the front live waiter. Committed entries deliver
//(pop + wake after the Sync release — the parked call resumes owning);
//pending entries stamp kAcquired and are NOT woken: the in-window
//acquirer's recheck consumes the stamp synchronously, so a wake here is
//redundant — and an unfair-GIL-delayed one lands after the acquirer
//consumed the stamp and parked on another primitive, flipping that
//unrelated park to Runnable and resuming it without its grant (observed
//as section over-admission, spurious unlock errors, and lost-permit
//deadlocks at >= 3 carriers). The Channel family's handoff arms never
//woke stamps — this restores that discipline. The committed wake is
//required and safe: its target is provably still in this park (or mid-
//unwind toward it), so the flip always lands on the right window.
bool VmExecutor::IntrinsicMutexUnlock(uint16_t callParamBase, uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Mutex call");
    const int selfId = m_spScheduler->CurrentContextId();
    int nextOwner = -1;
    bool committedHandoff = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        MutexSlot* slot = SyncMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Mutex call on an uninitialized instance");
        if (slot->ownerId != selfId)
            throw std::runtime_error(
                "NLang VM: Mutex.unlock without holding the lock");
        for (auto it = slot->waiters.begin(); it != slot->waiters.end();
             ++it) {
            if (it->phase != SyncWaiter::Phase::kPending
                && it->phase != SyncWaiter::Phase::kCommitted)
                continue;   //stamped entries are dead mailboxes
            nextOwner = it->contextId;
            if (it->phase == SyncWaiter::Phase::kCommitted) {
                slot->waiters.erase(it);
                committedHandoff = true;
                break;
            }
            it->phase = SyncWaiter::Phase::kAcquired;   //no wake: see above
            break;
        }
        if (nextOwner < 0)
            slot->ownerId = -1;   //no live waiter
        else
            slot->ownerId = nextOwner;
    }
    if (committedHandoff)
        m_spScheduler->WakeForMutex(nextOwner);
    return true;
}

//K8 shared: read the receiver's __handle from the heap (slot 0 = the
//instance). Heap-side reads are lock-free (D2 field reads) and the
//null-reference raise happens before any domain is taken. Returns 0
//for a never-constructed instance (the caller rejects it under Sync —
//the side-table bounds check is a structure read).
int32_t VmExecutor::ReadSyncPrimitiveHandle(uint16_t callParamBase,
                                            uint8_t* locals,
                                            const char* callName)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size()) {
        RaiseNlangException(m_nullPtrExcClassIdx,
            std::string("NLang VM: null reference in ") + callName);
    }
    return m_structHeap[static_cast<size_t>(thisIdx)]
                       [kListHandleFieldOffset];
}

//K8 shared: side-table lookup — call sites hold the Sync domain guard.
//The bounds check is a structure read (the store grows under Sync); a
//null return defers the uninitialized-instance error to the caller
//(thrown inside the section — a plain runtime_error acquires nothing).
VmExecutor::MutexSlot* VmExecutor::SyncMutexSlotAt(int32_t handle) {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_mutexStore.size())
        return nullptr;
    return &m_mutexStore[static_cast<size_t>(handle) - 1];
}

//Slice 8: allocate a mutex side-table slot (handle = index+1, 0
//reserved for null). Reuse resets the whole slot — the sweep only
//returns handles; owner/waiters may carry a discarded lock's state
//(the dying-while-holding program flaw). K8: the allocator owns the
//Sync-domain guard (the free-list pop and the growth are structure
//writes; reentrant for callers already inside the domain).
int32_t VmExecutor::AllocMutexHandle() {
    DomainGuard g(m_domainLocks, DomainId::Sync);
    if (!m_mutexFreeList.empty()) {
        const int32_t handle = m_mutexFreeList.back();
        m_mutexFreeList.pop_back();
        m_mutexStore[static_cast<size_t>(handle) - 1] = MutexSlot();
        return handle;
    }
    m_mutexStore.emplace_back();
    return static_cast<int32_t>(m_mutexStore.size());
}

//--- 0.10.21: the ReentrantMutex family — the Mutex shapes plus a
//per-owner hold count. lock()/tryLock() on the owning context take the
//re-entry arm (count + 1, never park, no scheduler state touched);
//unlock() decrements and only reaches the handoff arm at zero, where
//the new owner starts at a FRESH count of 1. Parking, handoff and the
//0.10.18 stamped-waiter discipline are byte-for-byte the Mutex arms.

bool VmExecutor::IntrinsicReentrantMutexCtor(uint16_t callParamBase,
                                             uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in ReentrantMutex "
                            "constructor");
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = AllocRMutexHandle();
    return true;
}

bool VmExecutor::IntrinsicReentrantMutexLock(uint16_t callParamBase,
                                             uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "ReentrantMutex call");
    const int selfId = m_spScheduler->CurrentContextId();
    bool mustPark = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        RMutexSlot* slot = SyncRMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: ReentrantMutex call on an uninitialized "
                "instance");
        if (slot->ownerId == selfId) {
            slot->count += 1;   //re-entry: no park, no scheduler state
        } else if (slot->ownerId < 0) {
            slot->ownerId = selfId;
            slot->count = 1;
        } else {
            //Gate BEFORE mutating scheduler state (decision 2 spec (a)):
            //the nested-window rejection must not leave a half-parked
            //context.
            EnsureSuspensionAllowed();
            slot->waiters.push_back(
                SyncWaiter{selfId, SyncWaiter::Phase::kPending});
            mustPark = true;
        }
    }
    if (!mustPark)
        return true;
    m_spScheduler->ParkForMutex(selfId);
    ConsumeRMutexParkStamp(handle, selfId);   //commits or consumes a stamp
    m_spScheduler->AbortPark(selfId);         //stamp path: never stole
    return true;
}

//The recheck half — the mutex stamp consumer against the RMutex store.
void VmExecutor::ConsumeRMutexParkStamp(int32_t handle, int selfId)
{
    DomainGuard g(m_domainLocks, DomainId::Sync);
    RMutexSlot* slot = SyncRMutexSlotAt(handle);
    if (!slot)
        throw std::runtime_error(
            "NLang VM: ReentrantMutex call on an uninitialized instance");
    ConsumeSyncWaiterStamp(slot->waiters, selfId,
                           ParkSignal::Reason::Mutex);
}

bool VmExecutor::IntrinsicReentrantMutexTryLock(uint16_t callParamBase,
                                                uint8_t* locals,
                                                uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "ReentrantMutex call");
    const int selfId = m_spScheduler->CurrentContextId();
    int32_t r = 0;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        RMutexSlot* slot = SyncRMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: ReentrantMutex call on an uninitialized "
                "instance");
        if (slot->ownerId == selfId) {
            slot->count += 1;   //re-entrant probe: count + 1, no park
            r = 1;
        } else if (slot->ownerId < 0) {
            slot->ownerId = selfId;
            slot->count = 1;
            r = 1;
        }
    }
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

bool VmExecutor::IntrinsicReentrantMutexUnlock(uint16_t callParamBase,
                                               uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "ReentrantMutex call");
    const int selfId = m_spScheduler->CurrentContextId();
    int nextOwner = -1;
    bool committedHandoff = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        RMutexSlot* slot = SyncRMutexSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: ReentrantMutex call on an uninitialized "
                "instance");
        if (slot->ownerId != selfId)
            throw std::runtime_error(
                "NLang VM: ReentrantMutex.unlock without holding the "
                "lock");
        //Owner check precedes the count: a balanced decrement that
        //still holds (count > 0) simply returns.
        if (--slot->count > 0)
            return true;
        for (auto it = slot->waiters.begin(); it != slot->waiters.end();
             ++it) {
            if (it->phase != SyncWaiter::Phase::kPending
                && it->phase != SyncWaiter::Phase::kCommitted)
                continue;   //stamped entries are dead mailboxes
            nextOwner = it->contextId;
            if (it->phase == SyncWaiter::Phase::kCommitted) {
                slot->waiters.erase(it);
                committedHandoff = true;
                break;
            }
            it->phase = SyncWaiter::Phase::kAcquired;   //no wake: see the
                                                        //Mutex arm above
            break;
        }
        if (nextOwner < 0) {
            slot->ownerId = -1;   //no live waiter
            slot->count = 0;
        } else {
            slot->ownerId = nextOwner;   //the grant starts a FRESH count
            slot->count = 1;
        }
    }
    if (committedHandoff)
        m_spScheduler->WakeForMutex(nextOwner);
    return true;
}

//0.10.21 shared: side-table lookup — call sites hold the Sync domain
//guard. Same contract as SyncMutexSlotAt.
VmExecutor::RMutexSlot* VmExecutor::SyncRMutexSlotAt(int32_t handle) {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_rmutexStore.size())
        return nullptr;
    return &m_rmutexStore[static_cast<size_t>(handle) - 1];
}

//0.10.21: allocate a reentrant-mutex side-table slot. Same shape as
//AllocMutexHandle — reuse resets the whole slot (owner, count, waiters),
//so a swept handle cannot leak the dying lock's hold count.
int32_t VmExecutor::AllocRMutexHandle() {
    DomainGuard g(m_domainLocks, DomainId::Sync);
    if (!m_rmutexFreeList.empty()) {
        const int32_t handle = m_rmutexFreeList.back();
        m_rmutexFreeList.pop_back();
        m_rmutexStore[static_cast<size_t>(handle) - 1] = RMutexSlot();
        return handle;
    }
    m_rmutexStore.emplace_back();
    return static_cast<int32_t>(m_rmutexStore.size());
}

} // namespace nlang
