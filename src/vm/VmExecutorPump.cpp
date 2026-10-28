/*---
    VmExecutorPump.cpp — P2-4 nested pump: a toString-override or host
    bridge window absorbs ParkSignal and drives the scheduler on this
    thread until its own window frames complete. CarrierLoop is the
    outermost pump (baseDepth 0, no pin); this is the same dispatch
    shape sliced above the pinned outer frames. Single dispatch path:
    EVERYTHING (the woken window included) comes off PopNextRunnable —
    no second dispatcher, no double-resume race. Own file so the loop
    and its helpers stay under the per-function line ceilings.
---*/
#include "VmExecutor.h"

namespace nlang {

//One served context's turn ABOVE the pinned frames: full dispatch via
//RunContextWindow(baseDepth), with CarrierLoop's four-arm catch shape
//sliced to this depth. A fatal error destroys the run (the pinned
//frames die with the eval-stack clear — the K1 exit policy).
void VmExecutor::PumpServeOne(int ctx, ExecutionContext* info,
                              size_t baseDepth) {
    try {
        m_spScheduler->ResetCarrierTripBudget(m_schedTripLimit);
        PumpServeScope gcLive(*this);
        RunContextWindow(ctx, info, *m_pumpMainFunc, baseDepth);
    }
    catch (ParkSignal&)
    {
        //Same steal model as CarrierLoop, sliced: the served context's
        //frames above baseDepth land in ctx->frames; the pinned frames
        //stay here. Runnable (quota preempt / woken mid-unwind)
        //re-enqueues; parked flavors arm the waker handshake.
        m_spScheduler->ParkContextFramesSlice(ctx, EvalStack(),
                                              baseDepth,
                                              /*enqueueIfRunnable=*/true);
    }
    catch (NLangThrow& ex)
    {
        //Containment unwinds only the served context's frames (the
        //throw surfaces from RunFrames(baseDepth)); the pinned window
        //and this pump continue.
        if (FailContextContained(ctx, ex.heapIdx))
            throw;   //main failed: propagate like CarrierLoop
    }
    catch (ShutdownSignal&)
    {
        //Translate: a normal return would hand the C++ caller an
        //uninitialized result cell.
        throw std::runtime_error(
            "NLang VM: run stopped while a nested window was suspended");
    }
    catch (...)
    {
        FailContextFatal(ctx);   //[[noreturn]]
    }
}

void VmExecutor::ResumePinnedWindow(int ctx, size_t baseDepth) {
    {
        //K7: the splice, the handshake clear and the rethrow arming are
        //the same locked transition RunContextWindow performs; the
        //bytecode below runs GIL-free (INV-1').
        GilEnsureGuard gil(*this);
        ExecutionContext* info = m_spScheduler->FindMutable(ctx);
        EvalStack().insert(EvalStack().end(),
            std::make_move_iterator(info->frames.begin()),
            std::make_move_iterator(info->frames.end()));
        info->frames.clear();
        info->framesParked = false;
        ArmResumeRethrow(info);
    }
    RunFrames(baseDepth);
}

void VmExecutor::RunNestedWindow(size_t baseDepth) {
    const int selfId = m_spScheduler->CurrentContextId();
    //The guard covers ONLY the fast-path RunFrames: the window's own
    //frames keep the nested exemption (quota skip, as before), while
    //the pump's served runs execute at nest depth 0 — full quota and
    //root-depth GC polls, exactly like carrier dispatch.
    try
    {
        NestEvalGuard nestedWindow(m_spScheduler->CarrierNestDepthRef());
        RunFrames(baseDepth);
        return;   //fast path: no suspension inside the window
    }
    catch (ParkSignal&)
    {
        //Slice the window's frames into the context; the outer frames
        //(and every live C++ frame above them) stay pinned here. The
        //context re-enters dispatch THROUGH THE QUEUE — same handshake
        //as any park. The pin lands before the slice: between them a
        //foreign carrier could pop the parked context.
        PumpPinScope pin(*this, selfId);
        m_spScheduler->ParkContextFramesSlice(selfId, EvalStack(),
                                              baseDepth,
                                              /*enqueueIfRunnable=*/true);
        PumpUntilWindowDone(selfId, baseDepth);
    }
}

//The pump loop: dispatch everything off the queue until OUR window's
//frames come home. Fairness is the queue's FIFO — a yield-looping
//window re-enqueues behind others, never starves them. Runs at nest
//depth 0 (the guard unwound with the caught signal).
void VmExecutor::PumpUntilWindowDone(int selfId, size_t baseDepth) {
    for (;;)
    {
        const int next = m_spScheduler->PopNextRunnable();
        //< 0 = stop requested: translate — a normal return would hand
        //the C++ caller an uninitialized result cell.
        if (next < 0)
            throw std::runtime_error(
                "NLang VM: run stopped while a nested window was "
                "suspended");
        if (next == selfId)
        {
            try
            {
                ResumePinnedWindow(selfId, baseDepth);
                if (EvalStack().size() == baseDepth)
                    return;   //window complete: result sits in its cell
            }
            catch (ParkSignal&)
            {
                //The resumed window parked AGAIN (a later yield/sleep in
                //the same toString body): re-slice and re-queue — same
                //handshake, pin still held.
                m_spScheduler->ParkContextFramesSlice(selfId, EvalStack(),
                    baseDepth, /*enqueueIfRunnable=*/true);
            }
        }
        else
        {
            PumpServeOne(next, m_spScheduler->FindMutable(next),
                         baseDepth);
        }
    }
}

} // namespace nlang
