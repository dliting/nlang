/*---
    IntrinsicsSemaphore.cpp — P2-1 (v0.10.17): the native Semaphore
    intrinsic family (ctor, acquire/release/tryAcquire). Same domain
    discipline as the IntrinsicsMutex.cpp family: structure sections
    under the Sync domain, scheduler calls between the sections; the
    waiter shape and the stamp consumption are shared with the Mutex
    store (SyncWaiter, ConsumeSyncWaiterStamp).
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//P2-1: Semaphore(n) construction — materialize the side-table slot and
//write the handle into __handle (the Channel ctor shape: slot 1 = the
//initial permit count; 0 is legal, negative raises the catchable
//error, the Exception base).
bool VmExecutor::IntrinsicSemaphoreCtor(uint16_t callParamBase,
                                        uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in Semaphore constructor");
    int32_t initial;
    std::memcpy(&initial, locals + callParamBase + kFrameSlotBytes,
                sizeof(initial));
    if (initial < 0)
        RaiseNlangException(m_exceptionClassIdx,
            "semaphore initial permit count must be at least 0");
    //The count write joins the allocation inside the allocator's Sync
    //section — the Channel capacity shape (the minted handle is not
    //yet visible, but a concurrent growth reallocates the store
    //vector, so the slot write shares the structure section).
    const int32_t handle = AllocSemaphoreHandle(initial);
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = handle;
    return true;
}

//P2-1: allocate a semaphore side-table slot (handle = index+1, 0
//reserved for null). The initial count is written inside the
//allocator's Sync section (the AllocChannelHandle capacity shape);
//reuse resets the whole slot — the sweep only returns handles, the
//count/waiters of a discarded semaphore must not leak into the reuse.
int32_t VmExecutor::AllocSemaphoreHandle(int initialCount) {
    DomainGuard g(m_domainLocks, DomainId::Sync);
    if (!m_semaphoreFreeList.empty()) {
        const int32_t handle = m_semaphoreFreeList.back();
        m_semaphoreFreeList.pop_back();
        SemaphoreSlot& slot =
            m_semaphoreStore[static_cast<size_t>(handle) - 1];
        slot = SemaphoreSlot();
        slot.count = initialCount;
        return handle;
    }
    m_semaphoreStore.emplace_back();
    m_semaphoreStore.back().count = initialCount;
    return static_cast<int32_t>(m_semaphoreStore.size());
}

//acquire(): count > 0 takes one; zero parks FIFO and release hands the
//permit to the front waiter directly (the count is unchanged by the
//handoff — the permit never enters the counter). The K8 two-phase
//handshake is the Mutex lock shape: register kPending under Sync,
//migrate the state, recheck — still kPending commits the park, a
//kAcquired stamp means a release already handed the permit to us
//inside the registration→migration window (abort the migration and
//return holding it).
bool VmExecutor::IntrinsicSemaphoreAcquire(uint16_t callParamBase,
                                           uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Semaphore call");
    const int selfId = m_spScheduler->CurrentContextId();
    bool mustPark = false;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        SemaphoreSlot* slot = SyncSemaphoreSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Semaphore call on an uninitialized instance");
        if (slot->count > 0) {
            --slot->count;
        } else {
            //Gate BEFORE mutating scheduler state: the nested-window
            //rejection must not leave a half-parked context.
            EnsureSuspensionAllowed();
            slot->waiters.push_back(
                SyncWaiter{selfId, SyncWaiter::Phase::kPending});
            mustPark = true;
        }
    }
    if (!mustPark)
        return true;
    m_spScheduler->ParkForSemaphore(selfId);
    ConsumeSemaphoreParkStamp(handle, selfId);   //commits or consumes
    m_spScheduler->AbortPark(selfId);            //stamp path: never stole
    return true;
}

//release(): no ownership precondition (a permit may be released without
//holding one — the count may exceed the initial value). A live front
//waiter receives the permit directly — committed entries pop + wake
//after the release, pending entries stamp kAcquired (the aborting
//acquirer returns holding it) — otherwise the count gains one. The
//dead-mailbox skip is the unlock shape: stamped entries never
//re-receive.
bool VmExecutor::IntrinsicSemaphoreRelease(uint16_t callParamBase,
                                           uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Semaphore call");
    int nextOwner = -1;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        SemaphoreSlot* slot = SyncSemaphoreSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Semaphore call on an uninitialized instance");
        for (auto it = slot->waiters.begin(); it != slot->waiters.end();
             ++it) {
            if (it->phase != SyncWaiter::Phase::kPending
                && it->phase != SyncWaiter::Phase::kCommitted)
                continue;   //stamped entries are dead mailboxes
            nextOwner = it->contextId;
            if (it->phase == SyncWaiter::Phase::kCommitted) {
                slot->waiters.erase(it);
                break;
            }
            it->phase = SyncWaiter::Phase::kAcquired;
            break;
        }
        if (nextOwner < 0)
            ++slot->count;   //no live waiter — bank the permit
    }
    if (nextOwner >= 0)
        m_spScheduler->WakeForSemaphore(nextOwner);
    return true;
}

//tryAcquire(): count > 0 takes one and returns true, zero returns
//false — never parks (the tryLock property: safe inside a nested
//evaluation window).
bool VmExecutor::IntrinsicSemaphoreTryAcquire(uint16_t callParamBase,
                                              uint8_t* locals,
                                              uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "Semaphore call");
    int32_t r = 0;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        SemaphoreSlot* slot = SyncSemaphoreSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: Semaphore call on an uninitialized instance");
        if (slot->count > 0) {
            --slot->count;
            r = 1;
        }
    }
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//P2-1: the semaphore recheck half — the ConsumeMutexParkStamp shape
//over the semaphore store (delegates to the shared stamp consumer).
void VmExecutor::ConsumeSemaphoreParkStamp(int32_t handle, int selfId)
{
    DomainGuard g(m_domainLocks, DomainId::Sync);
    SemaphoreSlot* slot = SyncSemaphoreSlotAt(handle);
    if (!slot)
        throw std::runtime_error(
            "NLang VM: Semaphore call on an uninitialized instance");
    ConsumeSyncWaiterStamp(slot->waiters, selfId,
                           ParkSignal::Reason::Semaphore);
}

//P2-1 shared: semaphore side-table lookup — call sites hold the Sync
//domain guard (the SyncMutexSlotAt shape).
VmExecutor::SemaphoreSlot* VmExecutor::SyncSemaphoreSlotAt(int32_t handle) {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_semaphoreStore.size())
        return nullptr;
    return &m_semaphoreStore[static_cast<size_t>(handle) - 1];
}

} // namespace nlang
