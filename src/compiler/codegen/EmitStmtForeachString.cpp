/*---
    EmitStmtForeachString.cpp — string-arm foreach emission
    (code-point iteration). Split out of EmitStmtForeach.cpp at the
    0.13.9 final-review fix batch (source-size gate).
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include "EmitPrimOps.h"
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>

namespace nlang {

//0.7.5 string arm: code-point iteration. Hidden locals mirror the
//index-based expansion in kind role (string handle / byte offset /
//continue flag — the counter-and-length pair collapses into the
//offset, since OP_StrForeachStep detects end-of-string itself).
//The loop head IS the step (decode + advance + continue flag in one
//op): continue jumps to the head (the step is the increment), break
//to the loop end via the miss-jump. No NullCheck — StrVal(0) reads
//as "" and the first step clears the flag; no EvalAreaClaim — the
//step touches only these locals (the walker's 2-slot foreach
//reservation stays a harmless over-estimate).
//Hidden locals for the string arm — the user char var (promoted kind
//when a body & binding flags the statement), the source handle, the
//byte offset and the continue flag; uniquified via the foreach counter.
VmBackend::StringForeachSlots VmBackend::AllocStringForeachLocals(
    SnForeachStmt& fe) {
    StringForeachSlots slots;
    uint16_t counter = m_currFunc->foreachCounter++;
    const uint8_t userVarKind = fe.ContainFlags(NF_Promoted)
        ? static_cast<uint8_t>(RTK_Promoted)
        : static_cast<uint8_t>(RTK_Char);
    //0.13.10 Task 9: the loop-var slot keys on the published descriptor
    //so a shadowed outer binding keeps its own storage (AllocDeclSlot).
    slots.userVarSlot = AllocDeclSlot(fe.VarLocal(), fe.VarName(),
        userVarKind);
    slots.iterSlot = AllocLocal(
        "__foreach_iter_" + std::to_string(counter),
        kFrameSlotBytes, RTK_String, false);
    slots.offSlot = AllocLocal(
        "__foreach_off_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    slots.condSlot = AllocLocal(
        "__foreach_cond_" + std::to_string(counter),
        kFrameSlotBytes, RTK_Int32, false);
    return slots;
}

void VmBackend::EmitStringForeach(SnForeachStmt& fe,
                                  BytecodeEmitter& emitter) {
    StringForeachSlots slots = AllocStringForeachLocals(fe);
    //Evaluate the source once; the byte offset starts at 0.
    EmitExpression(*fe.Iterable(), emitter, slots.iterSlot);
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(0);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slots.offSlot);
    //Loop head = the step. Miss-jump doubles as the break target.
    size_t loopStart = emitter.CurrentOffset();
    PushLoopContext();
    emitter.Emit(OpCode::OP_StrForeachStep);
    emitter.EmitUint16(slots.iterSlot);
    emitter.EmitUint16(slots.offSlot);
    emitter.EmitUint16(slots.condSlot);
    emitter.EmitUint16(slots.userVarSlot);
    emitter.Emit(OpCode::OP_JumpIfNot);
    size_t jumpToEnd = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder, patched below
    emitter.EmitUint16(slots.condSlot);
    m_loopStack.back().breakJumps.push_back(jumpToEnd);
    //0.13.10 Task 9: the body scope re-activates per iteration — the
    //step just landed the code point in the raw loop slot; activate the
    //fresh display, then copy the slot into it before the body's
    //captures read through the display field.
    EmitLoopDisplayActivation(*fe.BodyParagraph(), false, emitter);
    //Promoted loop var (char bound by & in the body): the step just
    //wrote the code point into the slot — wrap before any body read.
    if (fe.ContainFlags(NF_Promoted)) {
        auto *pCharType = fe.VarType()->Field();
        NLANG_ASSERT(pCharType);
        EmitPromotedValueWrap(slots.userVarSlot, pCharType->Kind(),
            emitter);
    }
    EmitLoopDisplayHeadCopies(*fe.BodyParagraph(), emitter);
    EmitStatement(*fe.Body(), emitter);
    emitter.Emit(OpCode::OP_Jump);
    emitter.EmitUint16(static_cast<uint16_t>(loopStart));
    //continue target = the head (the step advances the offset).
    size_t loopEnd = emitter.CurrentOffset();
    auto& ctx = m_loopStack.back();
    for (size_t pos : ctx.breakJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
    for (size_t pos : ctx.continueJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(loopStart));
    m_loopStack.pop_back();
}

} //namespace nlang
