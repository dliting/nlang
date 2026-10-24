#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

VmExecutor::~VmExecutor() = default;

//The four container side tables (List/Dict/Mutex/Channel) share one
//reset shape: clear store + free list, cache the builtin class index
//(absent class → -1 sentinel).
template <typename Store>
static void ResetContainerSideTable(Store& store,
                                    std::vector<int32_t>& freeList,
                                    const CompiledModule& module,
                                    const char* className, int16_t* cachedIdx) {
    store.clear();
    freeList.clear();
    int idx = module.FindClass(className);
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
    CacheBuiltinClassIndexes(module);
}

//Phase 9d: cache Exception hierarchy class indices. These are required
//by RaiseNlangException to allocate the right subclass instance.
//All FindClass calls here use bare names deliberately: the builtin
//List/Dict/Exception family carries no owner tag, so its table keys
//stay bare even after phase 5 qualified user keys.
void VmExecutor::CacheBuiltinClassIndexes(const CompiledModule& module) {
    auto cacheExcClass = [&](const char* name, int16_t* out) {
        int idx = module.FindClass(name);
        *out = (idx >= 0) ? static_cast<int16_t>(idx) : -1;
    };
    cacheExcClass("Exception",                &m_exceptionClassIdx);
    cacheExcClass("NullPointerException",     &m_nullPtrExcClassIdx);
    cacheExcClass("DivByZeroException",       &m_divZeroExcClassIdx);
    cacheExcClass("IndexOutOfBoundsException", &m_oobExcClassIdx);
    cacheExcClass("AssertionException",       &m_assertExcClassIdx);
    cacheExcClass("IOException",              &m_ioExcClassIdx);
    cacheExcClass("InvalidCastException",     &m_invalidCastExcClassIdx);
    cacheExcClass("ChannelClosedException",   &m_channelClosedExcClassIdx);

    //K1 Slice 6: cache the Thread class index (spawn mints instances).
    int threadIdx = module.FindClass("Thread");
    m_threadClassIdx = (threadIdx >= 0) ? static_cast<int16_t>(threadIdx) : -1;
    //K3: same for the Future face of typed spawn.
    int futureIdx = module.FindClass("Future");
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
    m_stringObjs.Emplace();   //sentinel slot 0 (handle 0 = null)
    m_stringObjs[0].form = static_cast<StrObj::Form>(kStrFormDead);
    m_strMarkBits.push_back(false);
    m_constStrCache.clear();
    m_constStrCache.reserve(module.stringConstants.size());
    for (const std::string& s : module.stringConstants)
        m_constStrCache.push_back(MintConstantString(s));
    m_emptyStrHandle = MintConstantString("");
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
    ResetPerRunState(module);
    InitStringStore(module);
    InitStructHeap();
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
//K2 slice 2: the catch-all arm of CarrierLoop, extracted for the
//source-size guard. Non-NLang errors (VM-internal, or the deadlock
//guard) are fatal for the whole run regardless of context — propagate.
//The frames die with the run (the backtrace comes from UnwindFrames(),
//not the frame stack): clearing them lets the dispatch end uphold
//INV-5.
[[noreturn]] void VmExecutor::FailContextFatal(int ctx)
{
    //K7: the clear mutates the published evaluation stack (a registry
    //scan arm may read it) — take the global lock for the transition.
    GilEnsureGuard gil(*this);
    m_spScheduler->MarkFailed(ctx, 0);
    EvalStack().clear();
    throw;
}

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
                RunContextWindow(ctx, info, mainFunc);
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
    const CompiledFunction& mainFunc) {
    bool resumed;
    {
        //K7: the resume decision, the steal-back, the handshake-flag
        //clear, and the rethrow arming write scheduler state the
        //collector's scheduler-root scan reads — one lock scope covers
        //the transition; RunFrames itself executes GIL-free and stays
        //outside it.
        GilEnsureGuard gil(*this);
        resumed = !info->frames.empty();
        if (resumed) {
            EvalStack() = std::move(info->frames);
            info->frames.clear();
            info->framesParked = false;
            ArmResumeRethrow(info);
        }
    }
    if (resumed)
        RunFrames(0);
    else
        RunRootWindow(mainFunc,
                      reinterpret_cast<uint8_t*>(&info->resultCell));
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

//K1 Slice 6: containment for an uncaught NLang exception escaping a
//context root. Spawned contexts never reach the host with their
//failure — Failed + wake joiners (with rethrow), dead frames' unwind
//records dropped; the failure surfaces only through join.
bool VmExecutor::FailContextContained(int ctx, int32_t excHeapIdx) {
    m_spScheduler->MarkFailed(ctx, excHeapIdx);
    if (ctx == VmScheduler::kMainThreadId)
        return true;
    UnwindFrames().clear();   //dead context's records
    m_spScheduler->WakeJoiners(ctx, /*failed=*/true);
    return false;
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

std::string VmExecutor::FormatBacktrace() const {
    std::string out;
    std::string moduleName = m_currModule ? m_currModule->name : "<module>";
    //UnwindFrames() is innermost-first (innermost's destructor ran first).
    //v0.8.5: each frame names ITS compiled translation unit, not the
    //current module (single-file builds only looked right by accident).
    for (const auto& f : UnwindFrames())
        out += "  at "
            + FormatBacktraceFrame(f.funcName, f.sourceFile, f.currentLine,
                                   moduleName)
            + "\n";
    return out;
}

//v0.8.5: see the header. std::string concatenation, not snprintf: paths
//on Windows exceed no buffer and need no truncation policy.
std::string FormatBacktraceFrame(const std::string& funcName,
    const std::string& file, uint16_t line,
    const std::string& fallbackModule) {
    //Legacy spelling: "<module>.n" — the fallback owns the suffix so
    //callers pass the bare module name.
    const std::string loc = !file.empty() ? file : fallbackModule + ".n";
    return funcName + " (" + loc + ":"
        + (line != 0 ? std::to_string(static_cast<unsigned>(line))
                     : std::string("?")) + ")";
}
//Phase 9d: build a fresh Exception instance of the requested subclass,
//populate message + backtrace, then throw NLangThrow to unwind to the
//nearest catch handler (or top-level if none).
[[noreturn]] void VmExecutor::RaiseNlangException(int16_t classIdx,
                                                   const std::string& msg) {
    //K8: the exception build self-locks (one Container section); the
    //guard stays for FireOnThrow — the throw-site checkpoint takes the
    //scheduler domain lock (same family as OpThrow/OpRethrow).
    GilEnsureGuard g(*this);
    const int32_t heapIdx = BuildNlangException(classIdx, msg);
    //Debugger checkpoint at the throw site, before unwinding starts
    //(the full NLang stack is still alive).
    FireOnThrow();
    throw NLangThrow(heapIdx, msg);
}

//K2 slice 4: the building half of RaiseNlangException, split out so
//NativeRaiseException can root the fresh object before throwing —
//between the throw and the blocking arm's catch the GIL is away (the
//unwinding callback guard releases it), and a GC on another carrier
//must not sweep the in-flight exception.
int32_t VmExecutor::BuildNlangException(int16_t classIdx,
                                         const std::string& msg) {
    //K8: single Container-domain lock (the backtrace List slot); allocations and mints are forward.
    DomainGuard g(m_domainLocks, DomainId::Container);
    if (classIdx < 0)
        throw std::runtime_error("NLang VM: exception class not registered");
    int32_t heapIdx = AllocClassOnHeap(static_cast<uint16_t>(classIdx));

    //cell[1] = message. Mint a string object, store its handle.
    //Do NOT hold a reference to m_structHeap[heapIdx] across the List
    //allocation below — AllocClassOnHeap may push_back to m_structHeap and
    //invalidate the reference (vector resize).
    int32_t msgHandle = MintNewString(msg);
    m_structHeap[static_cast<size_t>(heapIdx)][1] = msgHandle;

    //cell[3] = backtrace (field 1 under the uniform 2-cell stride).
    //Allocate a List<string> and push one entry per active call frame,
    //innermost-first.
    int32_t listHeapIdx = -1;
    if (m_listClassIdx >= 0) {
        listHeapIdx = AllocClassOnHeap(static_cast<uint16_t>(m_listClassIdx));
        //Initialize List.__handle by allocating a fresh side-table slot.
        m_listStore.emplace_back();  //new empty ListSlot
        int32_t handle = static_cast<int32_t>(m_listStore.size());
        //__handle is the first field of List (slot[1]).
        m_structHeap[static_cast<size_t>(listHeapIdx)][1] = handle;
        //Populate the backtrace: walk the evaluation stack innermost-first.
        std::string moduleName = m_currModule ? m_currModule->name : "<module>";
        for (auto it = EvalStack().rbegin(); it != EvalStack().rend(); ++it) {
            //v0.8.5: real per-frame source file (same helper/format as
            //FormatBacktrace — the two surfaces must not drift). The
            //mint/box/store tail is the existing code, unchanged.
            const std::string frame = FormatBacktraceFrame(
                it->func ? it->func->name : std::string("<unknown>"),
                it->func ? it->func->sourceFile : std::string(),
                it->currentLine, moduleName);
            //Box the string so the List element is a real RTK_Boxed
            //record — the old raw pool index made e.backtrace.get(i)
            //raise "unbox on null/invalid reference".
            int32_t frameHandle = MintNewString(frame);
            int32_t boxed = AllocBoxedValue(RTK_String, frameHandle);
            m_listStore[static_cast<size_t>(handle) - 1].elements.push_back(boxed);
        }
    }
    //Now write listHeapIdx to cell[3] of the Exception. Safe because no
    //further allocations happen before the throw.
    if (listHeapIdx > 0)
        m_structHeap[static_cast<size_t>(heapIdx)][3] = listHeapIdx;
    return heapIdx;
}

}
