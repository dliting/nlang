/*---
    VmExecutorFrames.cpp — 帧机的帧构造/弹出/异常处理器走查
    （PushFrameCore 族、PopFrameWithWriteback、RecordUnwindAndPopFrame、
    TryEnterCatchHandler、FrameResultCell）
    2026-10-18 并发 K1 从 VmExecutorOps.cpp 抽取（文件行数守卫），
    零行为变化。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

namespace {

//Stage the parameter block into a freshly pushed frame. A malformed
//module (parameter block wider than the callee's frame) must fail
//loudly, not silently skip the copy — the delegate paths reject the
//same shape. The size check also guards the bound arm's shifted copy.
void StageFrameParams(CallFrame& fr, const CompiledFunction& callee,
    const uint8_t* callerLocals, uint16_t callParamBase,
    int32_t boundThisIdx)
{
    uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
    if (paramBytes > callee.localsSize)
        throw std::runtime_error(
            "NLang VM: callee locals smaller than the parameter "
            "block in PushFrameCore: " + callee.name);
    if (boundThisIdx > 0) {
        std::memcpy(fr.localsBuf.data(), &boundThisIdx, sizeof(boundThisIdx));
        uint16_t argBytes = paramBytes - kFrameSlotBytes;
        if (argBytes > 0)
            std::memcpy(fr.localsBuf.data() + kFrameSlotBytes,
                        callerLocals + callParamBase, argBytes);
    } else if (paramBytes > 0) {
        std::memcpy(fr.localsBuf.data(), callerLocals + callParamBase,
                    paramBytes);
    }
}

} // namespace

//Shared push core. boundThisIdx > 0 selects bound-method staging (the
//captured receiver rides at slot 0, caller args shift one right);
//otherwise params copy verbatim from the caller's callParamBase cells.
uint8_t* VmExecutor::PushFrameCore(const CompiledFunction& callee,
    uint16_t callParamBase, uint32_t outMask, uint8_t outShift,
    int32_t boundThisIdx)
{
    if (EvalStack().size() >= RECURSE_LIMIT)
        throw std::runtime_error("NLang VM: recursion limit exceeded");
    //Capture the caller's state BEFORE emplace_back: the CallFrame
    //reference does not survive vector growth, but its locals buffer
    //pointer does (per-frame vectors move-steal their buffer — that is
    //the move-stability invariant the whole frame machine rests on).
    const uint8_t* callerLocals = EvalStack().back().localsBuf.data();
    uint8_t* extResult = EvalStack().back().externalResult;
    int32_t resFrameIdx = EvalStack().back().resultFrameIdx;
    uint16_t resDstOff = EvalStack().back().resultDstOff;

    EvalStack().emplace_back();
    CallFrame& fr = EvalStack().back();
    fr.localsBuf.assign(callee.localsSize, 0);
    StageFrameParams(fr, callee, callerLocals, callParamBase, boundThisIdx);
    //Result-cell inheritance: the callee's return lands in the same cell
    //the caller's frame writes to (accumulator threading — a call's
    //result IS the current expression's result).
    fr.externalResult = extResult;
    fr.resultFrameIdx = resFrameIdx;
    fr.resultDstOff = resDstOff;
    fr.func = &callee;
    fr.outMask = outMask;
    fr.callParamBase = callParamBase;
    fr.outShift = outShift;
    //Safepoint at function entry (same position as the recursive engine:
    //the new frame is on EvalStack() with staged/zeroed locals).
    CheckGCSafepoint();
    return fr.localsBuf.data();
}

uint8_t* VmExecutor::PushCalleeFrame(const CompiledFunction& callee,
    uint16_t callParamBase, uint32_t outMask, uint8_t outShift)
{
    return PushFrameCore(callee, callParamBase, outMask, outShift, 0);
}

uint8_t* VmExecutor::PushBoundCalleeFrame(const CompiledFunction& callee,
    int32_t thisIdx, uint16_t callParamBase, uint32_t outMask)
{
    return PushFrameCore(callee, callParamBase, outMask, 1, thisIdx);
}

uint8_t* VmExecutor::PushRootFrame(const CompiledFunction& func,
    uint8_t* externalResult)
{
    //Window roots count against RECURSE_LIMIT too — the recursive engine's
    //depth counter covered every ExecuteFunction invocation, including
    //toString and host-bridge windows (a pathological native → CallByName
    //re-entry chain was bounded at 1000; keep that bound).
    if (EvalStack().size() >= RECURSE_LIMIT)
        throw std::runtime_error("NLang VM: recursion limit exceeded");
    EvalStack().emplace_back();
    CallFrame& fr = EvalStack().back();
    fr.localsBuf.assign(func.localsSize, 0);
    fr.externalResult = externalResult;
    fr.func = &func;
    CheckGCSafepoint();
    return fr.localsBuf.data();
}

void VmExecutor::PopFrameWithWriteback()
{
    CallFrame& fr = EvalStack().back();
    if (fr.outMask != 0) {
        //The new top is the caller that staged this call. outMask is
        //armed only by call-opcode pushes, so a caller frame always
        //exists below.
        CallFrame& caller = EvalStack()[EvalStack().size() - 2];
        for (uint32_t slot = 0; slot < 32; ++slot) {
            if (!(fr.outMask & (1u << slot)))
                continue;
            uint16_t srcOff = static_cast<uint16_t>(
                (slot + fr.outShift) * kFrameSlotBytes);
            if (srcOff + kFrameSlotBytes > fr.localsBuf.size())
                throw std::runtime_error(
                    "NLang VM: out parameter slot out of bounds");
            uint32_t dst = fr.callParamBase + slot * kFrameSlotBytes;
            std::memcpy(caller.localsBuf.data() + dst,
                        fr.localsBuf.data() + srcOff, kFrameSlotBytes);
        }
    }
    EvalStack().pop_back();
}

void VmExecutor::RecordUnwindAndPopFrame()
{
    CallFrame& frame = EvalStack().back();
    UnwindFrames().push_back({
        frame.func ? frame.func->name : std::string("<unknown>"),
        frame.func ? frame.func->sourceFile : std::string(),
        frame.currentLine});
    EvalStack().pop_back();
}

bool VmExecutor::TryEnterCatchHandler(CallFrame& frame, int32_t excHeapIdx)
{
    //Scan func.tryBlocks in declaration order; the first match (range +
    //type) handles it. frame.opPc = offset of the throwing instruction.
    const CompiledFunction& func = *frame.func;
    for (const auto& tb : func.tryBlocks) {
        if (frame.opPc < tb.startPc || frame.opPc >= tb.endPc)
            continue;
        if (tb.exceptionClassIdx != 0xFFFF &&
            !IsInstanceOrSubclass(excHeapIdx, tb.exceptionClassIdx))
            continue;
        //Match. Bind catch var, push handler exception (for throw;),
        //clear unwind frames (so a subsequent throw's backtrace starts
        //fresh), and set the resume point to the handler.
        std::memcpy(frame.localsBuf.data() + tb.catchLocalOff, &excHeapIdx,
                    sizeof(int32_t));
        frame.handlerExcStack.push_back(excHeapIdx);
        UnwindFrames().clear();
        frame.pc = tb.handlerPc;
        return true;
    }
    return false;
}

uint8_t* VmExecutor::FrameResultCell(const CallFrame& frame)
{
    if (frame.externalResult)
        return frame.externalResult;
    return EvalStack()[static_cast<size_t>(frame.resultFrameIdx)]
        .localsBuf.data() + frame.resultDstOff;
}

} // namespace nlang
