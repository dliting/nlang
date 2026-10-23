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

//K2 slice 4 thread-locals (declared in VmExecutor.h). K7: the GIL owner
//marker moved to VmScheduler.h (namespace scope, inline) — t_gilOwner
//references below resolve to it.
thread_local int VmExecutor::t_blockingNativeDepth = 0;
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
//K7: the caller holds the GIL by protocol (CallNative's ensure guard;
//park before owner->Release, restore after owner.Acquire), so the
//inline steal into the registry stays serialized against the
//collector — ParkContextFrames's own guard is NOT taken here.
void VmExecutor::ParkFramesForBlockingNative(ExecutionContext* ctx)
{
    ctx->frames.insert(ctx->frames.end(),
        std::make_move_iterator(EvalStack().begin()),
        std::make_move_iterator(EvalStack().end()));
    EvalStack().clear();
    m_spScheduler->MarkBlockedNative(ctx->id);
    ++t_blockingNativeDepth;
    //K7: leave the rendezvous denominator for the whole blocking
    //section. A stop in flight makes this park the thread's Rule B
    //arrival; if it is the LAST one, collect inline (the GIL held
    //here makes the collector's guard a nested no-op). A re-entrant
    //callback inside the native re-enters executing through the
    //bridge scope and re-parks through this same path (LIFO balance).
    if (m_spScheduler->GcStop().EndExecuting())
        CollectAndComplete();
}

//Restore exactly the stolen frames (a re-entrant host callback may
//have parked more under this one) and mark the context Runnable-held
//(no requeue: the carrier resumes in place; FIFO governs queue
//members only). K7: runs under the reacquired GIL (FinishBlocking-
//Native's owner.Acquire) — the registry mutation is serialized.
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
}

//K2 slice 4 (§2.5) — the blocking-native arm. Stages args and the result
//cell onto the carrier's C++ stack (the native sees only staging — no
//concurrent read channel into frame storage remains), steals the frame
//stack into the context (BlockedNative; the empty stack is what the
//registry scan sees while this thread sits non-executing in the native),
//releases the GIL through the thread's current holder (a store-op
//guard or a re-entrant callback's ensure guard — nesting works either
//way), runs the native, reacquires, and
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
//K7 order: rejoin the executing set BEFORE taking the global lock —
//Rule A parks on the rendezvous cv until an in-flight stop completes,
//and the last arriver's collection needs the global lock, so waiting
//under it would deadlock. A zombie carrier's BeginExecuting returns
//false harmlessly (entry retired by the inter-run Reset).
void VmExecutor::FinishBlockingNative(GilHolder& owner, uint64_t rootOwner,
    uint64_t savedRootOwner, ExecutionContext* ctx, size_t stolenDepth)
{
    m_spScheduler->GcStop().BeginExecuting();
    owner.Acquire();
    t_currentNativeRootOwner = savedRootOwner;
    RemoveNativeRoots(rootOwner);
    --t_blockingNativeDepth;
    //K6 D9: run-generation mismatch — this carrier's run was torn down
    //while the native ran (a detached zombie). Own root entries are
    //already gone (they pointed at this thread's live stack); exit
    //without touching the new run: the context registry was cleared by
    //Reset and ctx is stale. The throw unwinds to the dispatch's (or
    //bridge's) scope destructor, which completes the executing flip-off.
    if (m_spScheduler->CarrierRunGeneration()
            != m_spScheduler->RunGeneration())
        throw ShutdownSignal();
    //Run over: frames stay parked in the registry for the shutdown;
    //the same scope destructor above flips this thread off.
    if (m_spScheduler->StopRequested())
        throw ShutdownSignal();
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
