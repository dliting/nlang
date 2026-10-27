/*---
    IntrinsicsAtomic.cpp — 0.10.22: the AtomicInt intrinsic family
    (ctor, load/store, exchange, CAS, add family). Own TU: the sync
    families live one TU per class beyond the mutex budget (the
    500-line source guard). Every method takes the sync-category lock
    (DomainGuard, Sync id) — same-category operations serialize across
    carriers, which is where the atomicity guarantee comes from. No
    method parks: all are legal inside a nested evaluation window.
    Errors defer (DeferredNlangRaise) or pass through as plain
    runtime_errors; the handle read is heap-side (lock-free) and the
    null-reference raise happens before any lock is taken.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//0.10.22 dispatcher: the ReentrantMutex (0.10.21), Semaphore and
//AtomicInt families' ids route here from ExecuteIntrinsicSync's
//default arm (one forward keeps the caller's switch under the 50-line
//function budget). Unknown ids return false — the caller surfaces the
//unknown-intrinsic error.
bool VmExecutor::ExecuteIntrinsicAtomic(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    case INTR_ReentrantMutex_Ctor:
        return IntrinsicReentrantMutexCtor(callParamBase, locals);
    case INTR_ReentrantMutex_Lock:
        return IntrinsicReentrantMutexLock(callParamBase, locals);
    case INTR_ReentrantMutex_Unlock:
        return IntrinsicReentrantMutexUnlock(callParamBase, locals);
    case INTR_ReentrantMutex_TryLock:
        return IntrinsicReentrantMutexTryLock(callParamBase, locals,
                                              pResult);
    case INTR_Semaphore_Ctor:
        return IntrinsicSemaphoreCtor(callParamBase, locals);
    case INTR_Semaphore_Acquire:
        return IntrinsicSemaphoreAcquire(callParamBase, locals);
    case INTR_Semaphore_Release:
        return IntrinsicSemaphoreRelease(callParamBase, locals);
    case INTR_Semaphore_TryAcquire:
        return IntrinsicSemaphoreTryAcquire(callParamBase, locals, pResult);
    case INTR_AtomicInt_Ctor:
        return IntrinsicAtomicIntCtor(callParamBase, locals);
    case INTR_AtomicInt_Load:
        return IntrinsicAtomicIntLoad(callParamBase, locals, pResult);
    case INTR_AtomicInt_Store:
        return IntrinsicAtomicIntStore(callParamBase, locals);
    case INTR_AtomicInt_GetAndSet:
        return IntrinsicAtomicIntGetAndSet(callParamBase, locals, pResult);
    case INTR_AtomicInt_CompareExchange:
        return IntrinsicAtomicIntCompareExchange(callParamBase, locals,
                                                 pResult);
    case INTR_AtomicInt_GetAndAdd:
        return IntrinsicAtomicIntGetAndAdd(callParamBase, locals, pResult);
    case INTR_AtomicInt_AddAndGet:
        return IntrinsicAtomicIntAddAndGet(callParamBase, locals, pResult);
    case INTR_AtomicInt_IncrementAndGet:
        return IntrinsicAtomicIntIncrementAndGet(callParamBase, locals,
                                                 pResult);
    case INTR_AtomicInt_GetAndIncrement:
        return IntrinsicAtomicIntGetAndIncrement(callParamBase, locals,
                                                 pResult);
    default:
        return false;
    }
}

//ctor: materialize the side-table slot and write the handle into
//__handle (the Semaphore shape). The declaration's default parameter
//(`int initial = 0`) guarantees the argument slot is staged at the
//call site even for `AtomicInt()` — OP_CallIntrinsic carries no
//argument count, so an unstaged tail slot would read stale memory.
bool VmExecutor::IntrinsicAtomicIntCtor(uint16_t callParamBase,
                                        uint8_t* locals)
{
    int32_t thisIdx;
    std::memcpy(&thisIdx, locals + callParamBase, sizeof(thisIdx));
    if (thisIdx <= 0
        || static_cast<size_t>(thisIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference in AtomicInt "
                            "constructor");
    int32_t initial = 0;
    std::memcpy(&initial, locals + callParamBase + kFrameSlotBytes,
                sizeof(initial));
    const int32_t handle = AllocAtomicHandle();
    m_atomicStore[static_cast<size_t>(handle) - 1].value = initial;
    m_structHeap[static_cast<size_t>(thisIdx)][kListHandleFieldOffset]
        = handle;
    return true;
}

//load(): atomic read.
bool VmExecutor::IntrinsicAtomicIntLoad(uint16_t callParamBase,
                                        uint8_t* locals, uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t v;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        v = slot->value;
    }
    std::memcpy(pResult, &v, sizeof(v));
    return true;
}

//store(v): atomic write. arg0 lives at callParamBase + kFrameSlotBytes.
bool VmExecutor::IntrinsicAtomicIntStore(uint16_t callParamBase,
                                         uint8_t* locals)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t v;
    std::memcpy(&v, locals + callParamBase + kFrameSlotBytes, sizeof(v));
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        slot->value = v;
    }
    return true;
}

//getAndSet(v): exchange — write v, return the previous value.
bool VmExecutor::IntrinsicAtomicIntGetAndSet(uint16_t callParamBase,
                                             uint8_t* locals,
                                             uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t v, old;
    std::memcpy(&v, locals + callParamBase + kFrameSlotBytes, sizeof(v));
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        old = slot->value;
        slot->value = v;
    }
    std::memcpy(pResult, &old, sizeof(old));
    return true;
}

//compareExchange(expect, v): CAS — if the current value == expect,
//write v and return true; otherwise leave the value and return false.
//args: expect at callParamBase + kFrameSlotBytes, v at + 2 slots.
bool VmExecutor::IntrinsicAtomicIntCompareExchange(uint16_t callParamBase,
                                                   uint8_t* locals,
                                                   uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t expect, v;
    std::memcpy(&expect, locals + callParamBase + kFrameSlotBytes,
                sizeof(expect));
    std::memcpy(&v, locals + callParamBase + 2 * kFrameSlotBytes,
                sizeof(v));
    int32_t r = 0;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        if (slot->value == expect) {
            slot->value = v;
            r = 1;
        }
    }
    std::memcpy(pResult, &r, sizeof(r));
    return true;
}

//getAndAdd(delta): add, return the PREVIOUS value (Java getAndX).
bool VmExecutor::IntrinsicAtomicIntGetAndAdd(uint16_t callParamBase,
                                             uint8_t* locals,
                                             uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t d, old;
    std::memcpy(&d, locals + callParamBase + kFrameSlotBytes, sizeof(d));
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        old = slot->value;
        slot->value = old + d;
    }
    std::memcpy(pResult, &old, sizeof(old));
    return true;
}

//addAndGet(delta): add, return the NEW value (Java xAndGet).
bool VmExecutor::IntrinsicAtomicIntAddAndGet(uint16_t callParamBase,
                                             uint8_t* locals,
                                             uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t d, nv;
    std::memcpy(&d, locals + callParamBase + kFrameSlotBytes, sizeof(d));
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        slot->value += d;
        nv = slot->value;
    }
    std::memcpy(pResult, &nv, sizeof(nv));
    return true;
}

//incrementAndGet(): ++i — add 1, return the new value.
bool VmExecutor::IntrinsicAtomicIntIncrementAndGet(uint16_t callParamBase,
                                                   uint8_t* locals,
                                                   uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t nv;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        nv = ++slot->value;
    }
    std::memcpy(pResult, &nv, sizeof(nv));
    return true;
}

//getAndIncrement(): i++ — add 1, return the previous value.
bool VmExecutor::IntrinsicAtomicIntGetAndIncrement(uint16_t callParamBase,
                                                   uint8_t* locals,
                                                   uint8_t* pResult)
{
    const int32_t handle = ReadSyncPrimitiveHandle(callParamBase, locals,
                                                   "AtomicInt call");
    int32_t old;
    {
        DomainGuard g(m_domainLocks, DomainId::Sync);
        AtomicSlot* slot = SyncAtomicSlotAt(handle);
        if (!slot)
            throw std::runtime_error(
                "NLang VM: AtomicInt call on an uninitialized instance");
        old = slot->value++;
    }
    std::memcpy(pResult, &old, sizeof(old));
    return true;
}

//0.10.22 shared: side-table lookup — call sites hold the sync-category
//guard. Same contract as SyncMutexSlotAt.
VmExecutor::AtomicSlot* VmExecutor::SyncAtomicSlotAt(int32_t handle) {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_atomicStore.size())
        return nullptr;
    return &m_atomicStore[static_cast<size_t>(handle) - 1];
}

//0.10.22: allocate an atomic side-table slot (handle = index+1, 0
//reserved for null). Reuse resets the whole slot — the sweep only
//returns handles; a stale value must not leak into the fresh instance.
//The sync-category guard covers the free-list pop and the growth.
int32_t VmExecutor::AllocAtomicHandle() {
    DomainGuard g(m_domainLocks, DomainId::Sync);
    if (!m_atomicFreeList.empty()) {
        const int32_t handle = m_atomicFreeList.back();
        m_atomicFreeList.pop_back();
        m_atomicStore[static_cast<size_t>(handle) - 1] = AtomicSlot();
        return handle;
    }
    m_atomicStore.emplace_back();
    return static_cast<int32_t>(m_atomicStore.size());
}

} // namespace nlang
