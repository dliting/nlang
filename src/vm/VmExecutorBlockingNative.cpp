/*---
    VmExecutorBlockingNative.cpp — the blocking-native arm (K2 slice 4)
    Extracted from VmExecutorNativeHost.cpp (K6 Task 2, 2026-10-22) so
    the K6 native-root ownership + run-generation changes land in a file
    with headroom under the source-size guard; zero behavior change.
---*/
#include "VmExecutor.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <iterator>

namespace nlang {

namespace {
//K6 D11: per-blocking-arm identities (monotonic; 0 stays invalid).
std::atomic<uint64_t> g_nextNativeRootOwner{1};
}

//--- Native call bridge (K2 slice 4, §2.5) -----------------------------------
//The blocking protocol for natives that may wait on the outside world.

//K2 slice 4 thread-locals (declared in VmExecutor.h).
thread_local int VmExecutor::t_blockingNativeDepth = 0;
thread_local VmExecutor::GilHolder* VmExecutor::t_gilOwner = nullptr;
thread_local uint64_t VmExecutor::t_currentNativeRootOwner = 0;

void VmExecutor::RemoveNativeRoots(uint64_t owner) {
    m_nativeCallRoots.erase(
        std::remove_if(m_nativeCallRoots.begin(), m_nativeCallRoots.end(),
            [owner](const NativeCallRoot& r) { return r.owner == owner; }),
        m_nativeCallRoots.end());
}

//Compiler-side sanity ceiling for call parameter counts (SnData.h
//kMaxFuncParams — the VM must not include compiler AST headers, so the
//value is mirrored here and tied by this comment).
constexpr uint32_t kMaxNativeStagedParams = 64;

//Steal the carrier's frame stack into the context and mark it
//BlockedNative (invisible to PopNextRunnable, still GC-scanned).
void VmExecutor::ParkFramesForBlockingNative(ExecutionContext* ctx)
{
    ctx->frames.insert(ctx->frames.end(),
        std::make_move_iterator(EvalStack().begin()),
        std::make_move_iterator(EvalStack().end()));
    EvalStack().clear();
    m_spScheduler->MarkBlockedNative(ctx->id);
    ++t_blockingNativeDepth;
    //K6 D2: the blocking section suspends this thread's evaluation-
    //window membership (GIL away, frames parked — no bytecode runs; a
    //re-entrant callback inside the native re-enters RunFrames and
    //re-counts itself through a nested guard).
    m_spScheduler->GcStop().SuspendEvalWindow();
}

//Restore exactly the stolen frames (a re-entrant host callback may
//have parked more under this one) and mark the context Runnable-held
//(no requeue: the carrier resumes in place; FIFO governs queue
//members only).
void VmExecutor::RestoreStolenFrames(ExecutionContext* ctx,
                                     size_t stolenDepth)
{
    EvalStack().assign(
        std::make_move_iterator(ctx->frames.end()
            - static_cast<std::ptrdiff_t>(stolenDepth)),
        std::make_move_iterator(ctx->frames.end()));
    ctx->frames.erase(ctx->frames.end()
        - static_cast<std::ptrdiff_t>(stolenDepth), ctx->frames.end());
    m_spScheduler->MarkRunnableHeld(ctx->id);
    //K6 D2: bytecode execution resumes — rejoin the evaluation window
    //the park suspended (balanced even under nested blocking arms: each
    //park/restore pair runs on this thread in LIFO order).
    m_spScheduler->GcStop().ResumeEvalWindow();
}

//K2 slice 4 (§2.5) — the blocking-native arm. Stages args and the result
//cell onto the carrier's C++ stack (the native sees only staging — no
//concurrent read channel into frame storage remains), steals the frame
//stack into the context (BlockedNative; EvalStack() empty for the whole
//GIL release keeps INV-2 true mid-window), releases the GIL through the
//thread's current holder (window guard or a re-entrant callback's ensure
//guard — nesting works either way), runs the native, reacquires, and
//removes its own root entries by owner identity UNDER the lock: while the
//GIL is away the roots must stay armed, since a GC another carrier runs
//before we get the lock has to see the staged cells (the native may
//already have written its result into them — retracting before
//reacquiring would open a collect-the-result window). Stop decides
//between ShutdownSignal passthrough (run over: frames stay parked, no
//result consumption) and restore. Exceptions from the native
//(raiseException's NLangThrow, or a host hook escaping despite the
//contract) are held and rethrown AFTER the restore, so RunFrames'
//handler walk sees the full frame stack.
bool VmExecutor::RunBlockingNative(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult,
    const BlockingInvoke& invoke)
{
    const uint32_t paramCount = callee.paramCount;
    if (paramCount > kMaxNativeStagedParams)
        throw std::runtime_error("NLang VM: native parameter count "
                                 "exceeds the staging ceiling");
    ExecutionContext* ctx = m_spScheduler->FindMutable(
        m_spScheduler->CurrentContextId());
    if (!ctx) {
        //Embed by-name bridge outside a run: run holding the GIL.
        return invoke(locals + callParamBase, paramCount, pResult);
    }
    //Before arming roots: a throw past here must not leave root cells.
    GilHolder* owner = t_gilOwner;
    if (!owner)
        throw std::runtime_error("NLang VM: no GIL holder for blocking native");
    uint8_t stagedArgs[kMaxNativeStagedParams * kFrameSlotBytes];
    if (paramCount)
        std::memcpy(stagedArgs, locals + callParamBase,
                    static_cast<size_t>(paramCount) * kFrameSlotBytes);
    uint8_t stagedRet[kFrameSlotBytes] = {0};
    //K6 D11: entries carry the owning arm's id — the old size-watermark
    //truncation could erase a LIVE arm's staged roots on interleaving.
    const uint64_t rootOwner = g_nextNativeRootOwner.fetch_add(
        1, std::memory_order_relaxed);
    m_nativeCallRoots.push_back({stagedRet, 1u, 0, rootOwner});
    if (paramCount)
        m_nativeCallRoots.push_back({stagedArgs, paramCount, 0, rootOwner});
    const size_t stolenDepth = EvalStack().size();
    ParkFramesForBlockingNative(ctx);
    //Callback-minted handles attribute to this arm (nested arms restore).
    const uint64_t savedRootOwner = t_currentNativeRootOwner;
    t_currentNativeRootOwner = rootOwner;
    owner->Release();
    std::exception_ptr nativeExc;
    bool handled = true;
    try {
        handled = invoke(stagedArgs, paramCount, stagedRet);
    } catch (...) {
        nativeExc = std::current_exception();
    }
    FinishBlockingNative(*owner, rootOwner, savedRootOwner, ctx,
                         stolenDepth);
    if (nativeExc)
        std::rethrow_exception(nativeExc);
    std::memcpy(pResult, stagedRet, kFrameSlotBytes);
    return handled;
}

//K6: reacquire tail (see the header declaration comment for the split).
void VmExecutor::FinishBlockingNative(GilHolder& owner, uint64_t rootOwner,
    uint64_t savedRootOwner, ExecutionContext* ctx, size_t stolenDepth)
{
    owner.Acquire();
    t_currentNativeRootOwner = savedRootOwner;
    RemoveNativeRoots(rootOwner);
    --t_blockingNativeDepth;
    //K6 D9: run-generation mismatch — this carrier's run was torn down
    //while the native ran (a detached zombie). Own root entries are
    //already gone (they pointed at this thread's live stack); exit
    //without touching the new run: the context registry was cleared by
    //Reset and ctx is stale. Reaching this check implies a carrier slot
    //was installed (ctx lookup goes through CurrentContextId, which
    //lives in the slot).
    if (m_spScheduler->CarrierRunGeneration()
            != m_spScheduler->RunGeneration()) {
        //K6 D2: resume balances the park's suspend (the unwind drops
        //the RunFrames guard); then exit without touching the new run.
        m_spScheduler->GcStop().ResumeEvalWindow();
        throw ShutdownSignal();
    }
    //Run over: frames stay parked; EvalStack() is empty (INV-2).
    if (m_spScheduler->StopRequested()) {
        //K6 D2: resume balances the park's suspend (the unwind drops
        //the RunFrames guard); frames stay parked for the shutdown.
        m_spScheduler->GcStop().ResumeEvalWindow();
        throw ShutdownSignal();
    }
    RestoreStolenFrames(ctx, stolenDepth);
}

//K2 slice 4: mint rooting — see NativeCallRoot. Only roots inside a
//blocking section on this thread; callbacks on foreign threads stay
//unrooted (the ABI contract keeps callback results on the calling
//thread for the duration of the call).
void VmExecutor::RootMintedNativeHandle(int32_t handle)
{
    if (t_blockingNativeDepth > 0 && handle != 0)
        m_nativeCallRoots.push_back(
            {nullptr, 0, handle, t_currentNativeRootOwner});
}

} // namespace nlang
