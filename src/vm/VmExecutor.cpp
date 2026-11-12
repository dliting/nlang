#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <exception>
#include <thread>

namespace nlang {

//v11 R8: ~VmExecutor moved to VmExecutorNativeHost.cpp (it drives the
//module-state teardown defined there; this file is at the size budget).
//2026-11-12 source-size split: the backtrace/raise family
//(FormatBacktrace/FormatBacktraceFrame, RaiseNlangException/
//BuildNlangException) moved to VmExecutorRaise.cpp.

//The container side tables (List/Dict/Mutex/Channel/Semaphore) share
//one reset shape: clear store + free list, cache the builtin class
//index (absent class → -1 sentinel).
template <typename Store>
static void ResetContainerSideTable(Store& store,
                                    std::vector<int32_t>& freeList,
                                    const CompiledModule& module,
                                    const char* className, int16_t* cachedIdx) {
    store.clear();
    freeList.clear();
    //D8: bare first, then a unique package-qualified suffix — the
    //container classes leave the prelude's bare-key world in the
    //concurrency import redesign.
    int idx = module.FindClassBareOrUniqueSuffix(className);
    *cachedIdx = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
}

//2026-09-26 maintainability split: Execute's per-run phases. Bodies are
//verbatim extracts; Execute keeps the original operation order.
void VmExecutor::ResetPerRunState(const CompiledModule& module) {
    //K1 decision 5: scheduler state resets explicitly per run — the
    //context registry, run queue, and timer list all clear (the
    //embedding host may reuse one executor across runs). The trip
    //LIMIT survives (SetSchedulerTripForTests semantics); the budget
    //and counters reset.
    m_spScheduler->Reset();
    m_quotaFires = 0;
    m_pumpMainFunc = nullptr;   //P2-4: re-armed by the next CarrierLoop
    m_gcCollections = 0;   //K6 D7: per-run collection count
    //K7: rendezvous bookkeeping resets with the run — the pressure flag
    //re-arms at the next allocation, and the parks counter is per-run
    //telemetry (GcWorldStop's own state went with the scheduler Reset
    //above).
    m_gcPressure.store(false, std::memory_order_release);
    m_gcRendezvousParks.store(0, std::memory_order_relaxed);
    m_preemptSkips = 0;
    //K8 Task 6: domain-lock telemetry is per-run (one run's contention
    //must not bleed into the next — same policy as the GIL wait
    //counters the scheduler Reset clears).
    m_domainLocks.ClearTelemetry();
    ResumeThrowExcIdxRef() = 0;
    UnwindFrames().clear();
    m_lastBacktrace.clear();
    //K2 slice 1: confluence slot resets per run (an executor reuse must
    //not rethrow a stale auxiliary error).
    m_auxError = nullptr;
    m_auxBacktrace.clear();
    m_byteStreams.clear();
    m_byteStreamFreeList.clear();
    m_fileStreams.clear();
    m_fileStreamFreeList.clear();
    //Phase 11 Q9: fresh entropy per run; math.srand replaces it on demand.
    m_rng.seed(std::random_device{}());
    ResetContainerSideTable(m_listStore, m_listFreeList, module,
                            kBuiltinListTypeName, &m_listClassIdx);
    ResetContainerSideTable(m_dictStore, m_dictFreeList, module,
                            kBuiltinDictTypeName, &m_dictClassIdx);
    ResetContainerSideTable(m_mutexStore, m_mutexFreeList, module,
                            "Mutex", &m_mutexClassIdx);
    ResetContainerSideTable(m_channelStore, m_channelFreeList, module,
                            kBuiltinChannelTypeName, &m_channelClassIdx);
    ResetContainerSideTable(m_semaphoreStore, m_semaphoreFreeList, module,
                            "Semaphore", &m_semaphoreClassIdx);
    ResetContainerSideTable(m_rmutexStore, m_rmutexFreeList, module,
                            "ReentrantMutex", &m_rmutexClassIdx);
    ResetContainerSideTable(m_atomicStore, m_atomicFreeList, module,
                            "AtomicInt", &m_atomicClassIdx);
    CacheBuiltinClassIndexes(module);
}

//Phase 9d: cache Exception hierarchy class indices. These are required
//by RaiseNlangException to allocate the right subclass instance.
//D8: lookups go bare-or-unique-suffix — the Exception family stays
//prelude-owned (bare keys), ChannelClosedException leaves with the
//concurrency packages (package-qualified key), and one helper carries
//both worlds.
void VmExecutor::CacheBuiltinClassIndexes(const CompiledModule& module) {
    auto cacheExcClass = [&](const char* name, int16_t* out) {
        int idx = module.FindClassBareOrUniqueSuffix(name);
        *out = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
    };
    cacheExcClass("Exception",                &m_exceptionClassIdx);
    cacheExcClass("NullPointerException",     &m_nullPtrExcClassIdx);
    cacheExcClass("DivByZeroException",       &m_divZeroExcClassIdx);
    cacheExcClass("IndexOutOfBoundsException", &m_oobExcClassIdx);
    cacheExcClass("AssertionException",       &m_assertExcClassIdx);
    cacheExcClass("IOException",              &m_ioExcClassIdx);
    cacheExcClass("InvalidCastException",     &m_invalidCastExcClassIdx);
    cacheExcClass("InitializationException",  &m_initExcClassIdx);
    cacheExcClass("ChannelClosedException",   &m_channelClosedExcClassIdx);

    //K1 Slice 6: cache the Thread class index (spawn mints instances).
    int threadIdx = module.FindClassBareOrUniqueSuffix("Thread");
    m_threadClassIdx = (threadIdx >= 0) ? static_cast<int16_t>(threadIdx) : -1;
    //K3: same for the Future face of typed spawn.
    int futureIdx = module.FindClassBareOrUniqueSuffix("Future");
    m_futureClassIdx = (futureIdx >= 0) ? static_cast<int16_t>(futureIdx) : -1;
}

//String objectization: eagerly materialize every module string constant
//as an immortal Flat object (JVM constant-pool semantics — constants
//live as long as the execution). Value consumers resolve constant
//indexes through m_constStrCache and never see raw indexes.
void VmExecutor::InitStringStore(const CompiledModule& module) {
    m_stringObjs.clear();
    m_strMarkBits.clear();
    //K8: one shard per carrier plus the host fallback slot (embed
    //threads that allocate without a carrier id). CarrierThreadCount is
    //final by now — SetCarrierThreadCount precedes Execute.
    m_strShards.Configure(static_cast<size_t>(m_carrierThreadCount) + 1);
    m_shortStrTable.clear();
    //0.13.4 (ruling D26): the string domain has no null — slot 0 is the
    //immortal, interned empty string, so the zero bit pattern every
    //allocator already produces (frame locals, struct/class/array
    //records, static slots, erased generic slots) reads as "" with no
    //fill pass. Go's zero-value string is the model; reference-kind
    //slots keep 0 = null (strings reach them only boxed, via OP_Box).
    m_stringObjs.Emplace();   //slot 0: the empty string
    m_stringObjs[0].interned = true;
    m_stringObjs[0].immortal = true;
    m_strMarkBits.push_back(false);
    m_shortStrTable[""] = 0;  //the one interned entry that never dies
    m_constStrCache.clear();
    m_constStrCache.reserve(module.stringConstants.size());
    for (const std::string& s : module.stringConstants)
        m_constStrCache.push_back(MintConstantString(s));
}

//Initialize struct heap with sentinel at index 0, plus GC state.
void VmExecutor::InitStructHeap() {
    m_structHeap.clear();
    m_structHeap.Emplace();  //empty slot at index 0

    m_slotKinds.clear();
    m_slotKinds.Emplace(0);    //sentinel
    m_slotStructIdx.clear();
    m_slotStructIdx.Emplace(0);
    //K8: one shard per carrier plus the host fallback slot.
    m_heapShards.Configure(static_cast<size_t>(m_carrierThreadCount) + 1);
    m_gcPressure.store(false, std::memory_order_release);
    EvalStack().clear();
}

void VmExecutor::InitializeForRun(const CompiledModule& module) {
    //Reset per-run state up front: if the executor is reused (e.g. a
    //future REPL), an early throw below must not expose stale frames
    //or backtrace from a previous run.
    m_currModule = &module;
    m_classLayout.Build(module.classes);
    //v0.12.6 D11: interface owners continue the owner table after the
    //classes (kind-1 gates address them through OwnerByKind).
    m_staticSlots.Build(module.staticSlots.size(), module.classes.size(),
                        module.interfaces.size());
    ResetPerRunState(module);
    InitStringStore(module);
    InitStructHeap();
    //v0.12.6 D14: an initializer-less struct static's default is a
    //zeroed struct record (nested structs and compile-time field
    //defaults included) — the static-slot sibling of the allocation-
    //time default instance fields get. Materialized at run init, before
    //any context can observe the cell; the records are module-lifetime
    //roots (MarkStaticSlotRoots' RTK_Struct arm) from the first
    //collection on. Initializer-bearing slots keep D12: their zero
    //reads as uninitialized and the read throws until the initializer
    //stores.
    for (size_t i = 0; i < module.staticSlots.size(); ++i) {
        const CompiledStaticSlot& slot = module.staticSlots[i];
        if (slot.type.kind != RTK_Struct || slot.hasInitializer)
            continue;
        const int32_t heapIdx = AllocStructOnHeap(slot.type.typeIdx);
        std::memcpy(m_staticSlots.Cell(i), &heapIdx, sizeof(heapIdx));
    }
    //GIL removal batch 1 (§7.2): pre-warm the native registry so the
    //module table stays read-only while frames execute — every native
    //the merged module declares is resolved here, at run init. A native
    //whose module is unavailable must keep its exact lazy failure at
    //the first call: the loader does not cache failures, so the attempt
    //the call re-makes throws the identical diagnostic. CallNative's
    //lazy path stays as that cold fallback.
    for (const CompiledFunction& fn : module.functions) {
        if (!fn.isNative)
            continue;
        try {
            EnsureNativeAvailable(fn.name);
        } catch (const std::exception&) {
            //Tolerated by design (see above): the first call re-attempts
            //and reports.
        }
    }
}

int VmExecutor::Execute(const CompiledModule& module) {
    InitializeForRun(module);

    //Only the root module's entryPoint is meaningful; imported modules
    //carry -1 (and merged function-table indices would shift anyway), so
    //there is deliberately no by-name fallback here. (Order note: this
    //check used to sit between the reset and the store init; after the
    //extraction it runs after the full init — not externally observable,
    //as the throw happens before any observable side effect.)
    int mainIdx = module.entryPoint;
    if (mainIdx < 0
        || mainIdx >= static_cast<int32_t>(module.functions.size()))
        throw std::runtime_error("NLang VM: module has no entry point");

    const CompiledFunction& mainFunc = module.functions[mainIdx];
    //pResult contract is a full uniform frame cell (kFrameSlotBytes): the
    //callee's final OP_VarLocal return copy writes 8 bytes, so a narrow
    //host-side int would be overflowed (ASan-confirmed stack corruption).
    //The 4-byte main return value sits in the low half. K2 slice 1: the
    //result is routed through the scheduler registry (main's
    //ctx->resultCell — written by whichever carrier ran it) and read
    //here, on the Execute thread, after the run completes; main and
    //spawned contexts are symmetric now, and no carrier ever writes a
    //host stack cell.
    RunPrimaryCarrier(mainFunc);
    ReportUnobservedSpawnedFailures();
    const int64_t result64 =
        m_spScheduler->Find(VmScheduler::kMainThreadId)->resultCell;
    int32_t result;
    std::memcpy(&result, &result64, sizeof(result));
    return result;
}

//K1 Slice 10: see VmExecutor.h. The entryIdx trust level matches
//Execute()'s (a linked module's entryPoint; intrinsics have no callable
//body and grammar keeps main a plain function). K2 slice 1: same
//registry routing; the whole cell copies out to the host buffer —
//embedding contract unchanged (kFrameSlotBytes == sizeof(int64_t)).
void VmExecutor::ExecuteLoadedEntry(uint16_t entryIdx, uint8_t* resultCell) {
    if (!m_currModule || entryIdx >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid entry index");
    RunPrimaryCarrier(m_currModule->functions[entryIdx]);
    ReportUnobservedSpawnedFailures();
    const int64_t result64 =
        m_spScheduler->Find(VmScheduler::kMainThreadId)->resultCell;
    std::memcpy(resultCell, &result64, kFrameSlotBytes);
}

//K1 Slice 6 / K2 slice 1/2: the shared carrier loop (installation of
//the carrier slot, the active-count guard, and main's registration all
//belong to the callers — RunPrimaryCarrier/RunAuxiliaryCarrier).
//Contexts (main + spawned) come off the scheduler's FIFO run queue.
//Frame ownership follows the steal model (VmScheduler.h): a context's
//frames ARE the carrier's evaluation stack while it runs, and move into ctx->frames on
//every park — so dispatching a context is either a fresh main root
//(frames empty, primary carrier only) or moving its frames back and
//re-entering RunFrames at the parked pc. main's return ends the run
//(unfinished contexts are dropped silently — the K1 exit policy;
//MarkFinished on main also requests the pool stop). K7: the dispatch
//window no longer holds the global lock — the lock moves to store
//operations (INV-1') and the carrier instead declares itself EXECUTING
//for the whole dispatch (the registry rendezvous denominator); the
//bytecode between store operations runs GIL-free. PopNextRunnable
//takes and releases the lock internally; the empty-queue wait happens
//inside it (see VmScheduler.cpp).
//K2 slice 2: the catch-all arm of CarrierLoop (FailContextFatal) and
//the containment pair (FailContextContained / the 0.10.20 run-end
//report) live in VmExecutorFailure.cpp.
void VmExecutor::CarrierLoop(const CompiledFunction& mainFunc) {
    try {
        for (;;) {
            const int ctx = m_spScheduler->PopNextRunnable();
            if (ctx < 0)
                break;   //stop requested: the run is over
            //K2 slice 2: each dispatch re-arms this carrier's quota.
            m_spScheduler->ResetCarrierTripBudget(m_schedTripLimit);
            ExecutionContext* info = m_spScheduler->FindMutable(ctx);
            try {
                //K7: rendezvous denominator for the whole dispatch;
                //unwinding destroys the scope BEFORE the catch arms run,
                //so throws complete their arrival with frames on-stack.
                ExecutingScope executing(*this);
                RunContextWindow(ctx, info, mainFunc, /*baseDepth=*/0);
            } catch (ParkSignal&) {
                //Not an error: steal the whole frame stack into the
                //context (the steal model) in one critical section that
                //also decides the queue entry — still-Runnable (quota
                //preempt, or woken mid-unwind) re-enqueues after the
                //frames landed; parked flavors set the framesParked
                //handshake for their waker.
                m_spScheduler->ParkContextFrames(ctx, std::move(EvalStack()));
                continue;
            } catch (NLangThrow& ex) {
                if (FailContextContained(ctx, ex.heapIdx))
                    throw;   //main: outer catch snapshots the backtrace
                continue;
            } catch (ShutdownSignal&) {
                //Stop mid-blocking-native (K2) or a zombie-carrier
                //ejection (K7): frames stay parked / die with the slot;
                //the context is dropped per the K1 exit policy.
                break;
            } catch (...) {
                FailContextFatal(ctx);
            }
            //INV-5 (K7): the carrier's evaluation stack must be empty at
            //dispatch end.
            if (!EvalStack().empty())
                std::terminate();
            if (ctx == VmScheduler::kMainThreadId)
                break;   //main returned: the run is over
        }
    } catch (const std::exception&) {
        //K7: shared executor state — write under the global lock (D8).
        GilEnsureGuard gil(*this);
        m_lastBacktrace = FormatBacktrace();
        throw;
    }
}

//K1 Slice 6: one context's turn on the carrier. Resume (a spawned
//context's first dispatch included) moves its frames back onto
//evaluation stack — RunFrames re-enters at each frame's parked pc, after
//the failed-join rethrow arm; a fresh main root goes through the root
//window. On return, record the result and wake joiners. K2 slice 1:
//every context, main included, writes its result into ctx->resultCell
//(registry cell — stable across carriers); the host reads it after the
//run. The fresh main root points its frame's result cell at the same
//registry slot (deque stability), so the pointer survives every steal.
void VmExecutor::RunContextWindow(int ctx, ExecutionContext* info,
    const CompiledFunction& mainFunc, size_t baseDepth) {
    bool resumed = SpliceResumedFrames(info, baseDepth);
    //Dispatch drain depth == splice boundary, always: the window ends
    //exactly where the splice began. A drain of 0 under a serve would
    //fall through into the pinned outer frames and re-dispatch the
    //suspended context mid-window (double dispatch — P2-4 review).
    try {
        if (resumed)
            RunFrames(baseDepth);
        else if (baseDepth == 0)
            RunRootWindow(mainFunc,
                          reinterpret_cast<uint8_t*>(&info->resultCell));
        else
            //Unreachable by construction (every queued context parks
            //with frames); a fresh root under a serve would build MAIN's
            //frame into the served context — refuse instead.
            throw std::runtime_error(
                "NLang VM: queued context has no frames to resume");
    } catch (NLangThrow& ex) {
        RootEscapingException(info, ex.heapIdx);
        throw;
    }
    m_spScheduler->MarkFinished(ctx, info->resultCell);
    //K3: fill get()-waiters' destinations BEFORE waking them — a parked
    //call is not re-executed on resume (the pc-advance invariant), so the
    //value must already be in place when the waiter's frame re-enters.
    m_spScheduler->DeliverJoinResults(ctx, info->resultCell);
    m_spScheduler->WakeJoiners(ctx, /*failed=*/false);
}

//K1 Slice 6: a joiner woken over a FAILED target rethrows the target's
//exception into its own frame stack before dispatch — ResumeThrowExcIdxRef()
//consumed at RunFrames entry, inside the handler-search try (the join
//call site's opPc matches, exactly as if join had thrown).
void VmExecutor::ArmResumeRethrow(ExecutionContext* info) {
    if (!info->rethrowOnResume)
        return;
    info->rethrowOnResume = false;
    //K3: the channel-close arm prebuilt the exception object and rooted
    //it on the parked context — throw it directly. The failed-join arm
    //below instead rethrows the TARGET's stored exception.
    if (info->resumeExcHeapIdx > 0) {
        ResumeThrowExcIdxRef() = info->resumeExcHeapIdx;
        info->resumeExcHeapIdx = 0;
        return;
    }
    const ExecutionContext* target = m_spScheduler->Find(info->parkHandle);
    info->parkHandle = 0;
    //Only failure arms the flag; a missing/exceptionless target is a
    //corrupt registry — refuse loudly, never silently no-op.
    if (!target || target->failExcHeapIdx <= 0)
        throw std::runtime_error(
            "NLang VM: join target failed without an exception object");
    ResumeThrowExcIdxRef() = target->failExcHeapIdx;
}

//K1 Slice 4: one context's root window — Execute's former run body,
//unchanged: native entries dispatch through the host table (a
//pathological `native int main();` must not silently return 0 on the
//declaration's empty bytecode; the zeroed staging cell keeps exact
//parity with the recursive engine's ABI), byte-code entries push the
//root frame and run to completion.
void VmExecutor::RunRootWindow(const CompiledFunction& entryFunc,
                               uint8_t* resultCell) {
    if (entryFunc.isNative) {
        std::vector<uint8_t> staged(entryFunc.localsSize, 0);
        CallNative(entryFunc, 0, staged.data(), resultCell);
    } else {
        //K7: no window guard here — the dispatch's ExecutingScope (the
        //carrier loop) already declared this thread executing, so the
        //root frame's function-entry safepoint counts it via the normal
        //poll.
        PushRootFrame(entryFunc, resultCell);
        RunFrames(0);
    }
}

}
