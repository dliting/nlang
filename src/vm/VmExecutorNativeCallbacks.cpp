/*---
VmExecutorNativeCallbacks.cpp — the non-array native-host callback
implementations: raiseException's exception-family mapping (K8, v6),
the PRNG draws (K8), and the ABI v11+ blocking-platform family (IO
parking, module state, work pool, receiver type-argument reads v12).
The array side (getArrayView's pinned payload views, the creating
family) lives in VmExecutorNativeArray.cpp (source-size-guard split,
zero behavior change). Table wiring stays in InitNativeHost.
---*/
#include "VmExecutor.h"
#include "VmExecutorNativeHost.h"

#include <chrono>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace nlang {

namespace {
//A native side holds NativeHost* equal to &vmNativeHost.c (the first
//member); restore the wrapper.
VmNativeHost* Wrap(NativeHost* self) {
    return reinterpret_cast<VmNativeHost*>(self);
}
} // namespace

void VmExecutor::NativeRaiseException(NativeHost* self, int exceptionKind,
                                      const char* message, int64_t len) {
    //v13: the message is `len` bytes, so a NUL byte inside it is content
    //like any other; a null pointer or non-positive length is the empty
    //message.
    const std::string text = (message != nullptr && len > 0)
        ? std::string(message, static_cast<size_t>(len))
        : std::string();
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    //K8: keeps the store lock — FireOnThrow is a scheduler-facing
    //checkpoint (the OpThrow family keeps its guard for the same
    //reason); restated with the scheduler lock in Step 7.
    //v6: native kernels raise the same catchable families the language
    //runtime does. A negative class index (builtin class absent from the
    //linked module — defensive, every module registers them) falls back
    //to the base Exception.
    int16_t classIdx = e->m_exceptionClassIdx;
    switch (exceptionKind) {
        case NEXC_IOException:   classIdx = e->m_ioExcClassIdx; break;
        case NEXC_NullPointer:   classIdx = e->m_nullPtrExcClassIdx; break;
        case NEXC_OutOfBounds:   classIdx = e->m_oobExcClassIdx; break;
        case NEXC_DivideByZero:  classIdx = e->m_divZeroExcClassIdx; break;
        default:                 classIdx = e->m_exceptionClassIdx; break;
    }
    if (classIdx < 0)
        classIdx = e->m_exceptionClassIdx;
    //Build-then-root-then-throw: between this throw and the blocking
    //arm's catch the GIL is away (the unwinding guard above releases
    //it), and a GC on another carrier must not sweep the in-flight
    //exception object while the native unwinds toward it. The build +
    //root span rides the native-creator gate (contract in VmExecutor.h)
    //so a collection cannot sweep the fresh object before its root
    //lands; FireOnThrow stays outside the gate (scheduler checkpoint).
    int32_t heapIdx = 0;
    {
        NativeCreationGate creationGate(*e);
        heapIdx = e->BuildNlangException(classIdx, text);
        e->RootCreatedNativeHandle(heapIdx);
    }
    e->FireOnThrow();
    throw NLangThrow(heapIdx, text);
}

uint32_t VmExecutor::NativeNextRandom(NativeHost* self) {
    VmExecutor* e = Wrap(self)->executor;
    //K8: the shared engine serializes draws and reseeds on the RNG
    //mutex (host-boundary leaf — the engine is not a store).
    std::lock_guard<std::mutex> rngGuard(e->m_rngMutex);
    return e->m_rng();
}

void VmExecutor::NativeSeedRandom(NativeHost* self, int32_t seed) {
    VmExecutor* e = Wrap(self)->executor;
    //K8: the shared engine serializes draws and reseeds on the RNG
    //mutex (host-boundary leaf — the engine is not a store).
    std::lock_guard<std::mutex> rngGuard(e->m_rngMutex);
    e->m_rng.seed(static_cast<uint32_t>(seed));
}


//--- ABI v11: IO parking, module state, work pool ----------------------
//(contracts in NativeHost.h; plan rulings R7/R8/R13/R14, r1 note 4)

long long VmExecutor::NativeNowMs(NativeHost* /*self*/) {
    //Same clock domain as the scheduler's IO-park deadlines
    //(VmSchedulerIo): steady_clock epoch, not wall time.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int VmExecutor::NativeParkForIo(NativeHost* self, int fd, unsigned events,
    long long deadlineMs, int hasDeadline) {
    VmExecutor* e = Wrap(self)->executor;
    //Entered from a blocking-channel native: the protocol released the
    //GIL around invoke, so this guard re-takes it (R7 contract).
    GilEnsureGuard gil(*e);
    VmScheduler* sched = e->m_spScheduler.get();
    const int selfId = sched->CurrentContextId();
    ExecutionContext* ctx = sched->FindMutable(selfId);
    //Re-entry after wake: WakeForIo armed this context; clear the flag
    //and answer 1 so the native retries its syscall.
    if (ctx && ctx->ioParkArmed) {
        ctx->ioParkArmed = false;
        return 1;
    }
    if (!ctx)
        return 0;                        //embed path: nothing to park
    //First entry: a park needs a carrier to release (same gate as the
    //intrinsic RequestPark path; throws before any state mutation —
    //the complete-or-park invariant).
    e->EnsureSuspensionAllowed();
    const bool has = hasDeadline != 0 && deadlineMs > 0;
    //No fd and no deadline: no readiness source and nothing to expire
    //— parking would hang with no possible wake. Answer ready instead
    //(the caller's wait is a programming error, not a deadlock;
    //final review M14).
    if (fd < 0 && !has)
        return 1;
    const std::chrono::steady_clock::time_point deadline{
        std::chrono::milliseconds(deadlineMs)};
    sched->ParkForIo(selfId, fd, events, deadline, has);
    //FinishBlockingNative sees the flag and unwinds with ParkSignal(Io)
    //instead of restoring frames: the carrier parks this context, and
    //the wake re-executes this CallNative with the same staged
    //arguments (R7).
    ctx->ioParkPending = true;
    return 0;
}

void* VmExecutor::NativeModuleState(NativeHost* self, const char* tag,
    void* (*ctor)(void), void (*dtor)(void*)) {
    VmExecutor* e = Wrap(self)->executor;
    //R8: one state slot per tag per executor. The leaf mutex serializes
    //first-touch construction (module inits can race across carriers).
    std::lock_guard<std::mutex> g(e->m_moduleStateMutex);
    const std::string key(tag);
    auto it = e->m_moduleStates.find(key);
    if (it != e->m_moduleStates.end())
        return it->second.first;
    void* state = ctor();
    e->m_moduleStates.emplace(key, std::make_pair(state, dtor));
    return state;
}

void VmExecutor::NativeWakeIoWaiters(NativeHost* self, int fd) {
    VmExecutor* e = Wrap(self)->executor;
    //R13: callable without the GIL (a worker thread closing a socket);
    //the scheduler method takes the lock internally.
    e->m_spScheduler->WakeIoWaitersForFd(fd);
}

int VmExecutor::NativeCurrentContextId(NativeHost* self) {
    VmExecutor* e = Wrap(self)->executor;
    //R14: the carrier-slot read needs no GIL. Stable across the park/
    //re-entry window: the re-executed CallNative re-dispatches on the
    //same context. -1 outside a blocking-channel native.
    return e->m_spScheduler->CurrentContextId();
}

int VmExecutor::NativeSubmitWork(NativeHost* self, void (*fn)(void*),
    void* arg, int ackFd, int priority) {
    VmExecutor* e = Wrap(self)->executor;
    //r1 note 4: the pool queue lock is independent of the GIL, so a
    //submit from a GIL-free blocking native (or a worker) is safe.
    return e->m_spScheduler->SubmitWork(fn, arg, ackFd, priority);
}

//--- v12: receiver type-argument reads ---------------------------------------
//The instantiation descriptor is read-only and interned (process-lifetime
//stable), so the three reads need no lock beyond the descriptor lookup —
//the same discipline the cast-dispatch and typeName consumers use.

uint32_t VmExecutor::NativeTypeArgCount(NativeHost* self) {
    VmNativeHost* h = Wrap(self);
    const GenericInstDescriptor* desc =
        h->executor->InstDescriptorOf(h->receiverHeapIdx);
    return desc ? static_cast<uint32_t>(desc->TypeArgs().size()) : 0;
}

uint8_t VmExecutor::NativeTypeArgKind(NativeHost* self, uint32_t i) {
    VmNativeHost* h = Wrap(self);
    const GenericInstDescriptor* desc =
        h->executor->InstDescriptorOf(h->receiverHeapIdx);
    if (!desc || i >= desc->TypeArgs().size())
        return 0xFF;   //the existing invalid-RTK sentinel
    return desc->TypeArgs()[i].resolvedKind;
}

const char* VmExecutor::NativeTypeArgName(NativeHost* self, uint32_t i) {
    VmNativeHost* h = Wrap(self);
    const GenericInstDescriptor* desc =
        h->executor->InstDescriptorOf(h->receiverHeapIdx);
    if (!desc || i >= desc->TypeArgs().size())
        return nullptr;   //the name sentinel
    return desc->TypeArgs()[i].spelling.c_str();
}

} // namespace nlang
