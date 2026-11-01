#include "VmExecutor.h"
#include <iostream>
#include <stdexcept>

namespace nlang {

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

//RunContextWindow's escape catch. Invariant: an NLang heap handle
//crossing a window boundary inside a C++ exception must be rooted in
//a scanned surface before the window's ExecutingScope retires. By the
//time an escape reaches the window the handler search has popped
//every frame of that window, so the exception object is reachable
//only from the C++ exception in flight — GC-invisible once the
//exception crosses the window and the dispatch scope's ExecutingScope
//retires. Publish it as the context's failure handle now, under the
//GIL (the same field MarkFailed rewrites later; every reader gates on
//state == Failed, which only MarkFailed sets, so this early value is
//purely a GC root until then — MarkSchedulerRoots scans it in
//ctxRoots).
void VmExecutor::RootEscapingException(ExecutionContext* info,
                                       int32_t heapIdx) {
    {
        GilEnsureGuard gil(*this);
        info->failExcHeapIdx = heapIdx;
    }
    //White-box knob: run one collection exactly here, while the
    //escaping exception is rooted only by the publish above and this
    //carrier is still inside the GC rendezvous set — proves the early
    //publish keeps the in-flight object alive across a real sweep.
    if (!m_gcCollectInFailureCatchForTests.exchange(
            false, std::memory_order_relaxed))
        return;
    GcWorldStop& stop = m_spScheduler->GcStop();
    if (stop.InitiateAndArrive())
        CollectAndComplete();
    else
        stop.WaitForCompletion();
    //InitiateAndArrive consumed this carrier's executing bit; the
    //dispatch scope still owns it and will EndExecuting when the
    //exception crosses the window — re-arm it (same shape as
    //CollectAndReenter's re-entry).
    stop.BeginExecuting();
}

//0.10.20: containment used to swallow a spawned task's failure when no
//joiner ever watched it — the run ended green with the error gone. The
//run-end report prints one stderr line per such context (Python's
//"Task exception was never retrieved" analogue; Java's default uncaught
//handler prints to stderr the same way). Carriers have joined by the
//time this runs, so the registry is quiesced; the global lock is still
//taken, matching every other teardown reader of shared state. The
//failed exception object stays alive through the run (the GC roots
//ctx->failExcHeapIdx), so the type name and message read safely here.
void VmExecutor::ReportUnobservedSpawnedFailures() {
    GilEnsureGuard gil(*this);
    const size_t count = m_spScheduler->ContextCount();
    for (size_t id = 0; id < count; ++id) {
        const ExecutionContext* ctx = m_spScheduler->Find(static_cast<int>(id));
        if (!ctx || ctx->state != ContextState::Failed
            || ctx->failObserved
            || ctx->failExcHeapIdx <= 0
            || ctx->id == VmScheduler::kMainThreadId)
            continue;
        std::string typeName = "exception";
        std::string message;
        const int32_t excIdx = ctx->failExcHeapIdx;
        if (excIdx < static_cast<int32_t>(m_structHeap.size())
            && m_structHeap[static_cast<size_t>(excIdx)].size() >= 2) {
            try {
                typeName = LeafNameOfKey(HostClassName(excIdx));
            } catch (const std::exception&) {
                //defensive: a malformed record still reports the task
            }
            message = StrValCopy(
                m_structHeap[static_cast<size_t>(excIdx)][1]);
        }
        std::cerr << "NLang VM: spawned task " << ctx->id
                  << " failed with uncaught " << typeName << ": "
                  << message << " (never joined)"
                  << FormatCauseChainTail(excIdx) << "\n";
    }
}

}  // namespace nlang
