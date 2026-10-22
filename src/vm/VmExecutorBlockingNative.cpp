/*---
    VmExecutorBlockingNative.cpp — the blocking-native arm (K2 slice 4)
    Extracted from VmExecutorNativeHost.cpp (K6 Task 2, 2026-10-22) so
    the K6 native-root ownership + run-generation changes land in a file
    with headroom under the source-size guard; zero behavior change.
---*/
#include "VmExecutor.h"

#include <cstring>
#include <exception>
#include <iterator>

namespace nlang {

//--- Native call bridge (K2 slice 4, §2.5) -----------------------------------
//The blocking protocol for natives that may wait on the outside world.

//K2 slice 4 thread-locals (declared in VmExecutor.h).
thread_local int VmExecutor::t_blockingNativeDepth = 0;
thread_local VmExecutor::GilHolder* VmExecutor::t_gilOwner = nullptr;

//Compiler-side sanity ceiling for call parameter counts (SnData.h
//kMaxFuncParams — the VM must not include compiler AST headers, so the
//value is mirrored here and tied by this comment).
constexpr uint32_t kMaxNativeStagedParams = 64;

//Steal the carrier's frame stack into the context and mark it
//BlockedNative (invisible to PopNextRunnable, still GC-scanned).
void VmExecutor::ParkFramesForBlockingNative(ExecutionContext* ctx)
{
    ctx->frames.insert(ctx->frames.end(),
        std::make_move_iterator(m_callStack.begin()),
        std::make_move_iterator(m_callStack.end()));
    m_callStack.clear();
    m_spScheduler->MarkBlockedNative(ctx->id);
    ++t_blockingNativeDepth;
}

//Restore exactly the stolen frames (a re-entrant host callback may
//have parked more under this one) and mark the context Runnable-held
//(no requeue: the carrier resumes in place; FIFO governs queue
//members only).
void VmExecutor::RestoreStolenFrames(ExecutionContext* ctx,
                                     size_t stolenDepth)
{
    m_callStack.assign(
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
//stack into the context (BlockedNative; m_callStack empty for the whole
//GIL release keeps INV-2 true mid-window), releases the GIL through the
//thread's current holder (window guard or a re-entrant callback's ensure
//guard — nesting works either way), runs the native, reacquires, and
//truncates the native roots to the watermark UNDER the lock: while the
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
    const size_t watermark = m_nativeCallRoots.size();
    m_nativeCallRoots.push_back({stagedRet, 1u, 0});
    if (paramCount)
        m_nativeCallRoots.push_back({stagedArgs, paramCount, 0});
    const size_t stolenDepth = m_callStack.size();
    ParkFramesForBlockingNative(ctx);
    owner->Release();
    std::exception_ptr nativeExc;
    bool handled = true;
    try {
        handled = invoke(stagedArgs, paramCount, stagedRet);
    } catch (...) {
        nativeExc = std::current_exception();
    }
    owner->Acquire();
    m_nativeCallRoots.resize(watermark);
    --t_blockingNativeDepth;
    if (m_spScheduler->StopRequested()) {
        //Run over: frames stay parked; m_callStack is empty (INV-2).
        throw ShutdownSignal();
    }
    RestoreStolenFrames(ctx, stolenDepth);
    if (nativeExc)
        std::rethrow_exception(nativeExc);
    std::memcpy(pResult, stagedRet, kFrameSlotBytes);
    return handled;
}

//K2 slice 4: mint rooting — see NativeCallRoot. Only roots inside a
//blocking section on this thread; callbacks on foreign threads stay
//unrooted (the ABI contract keeps callback results on the calling
//thread for the duration of the call).
void VmExecutor::RootMintedNativeHandle(int32_t handle)
{
    if (t_blockingNativeDepth > 0 && handle != 0)
        m_nativeCallRoots.push_back({nullptr, 0, handle});
}

} // namespace nlang
