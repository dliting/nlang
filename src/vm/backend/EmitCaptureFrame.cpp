/*---
    EmitCaptureFrame.cpp — 0.13.10 lambda capture emission, synthetic
    frame half. The captured-formal prologue copies (each formal's
    argument lands in its display field, flavor-dispatched) and the
    lambda literal's bound-func emission — the two emission moments of
    the frame the analysis synthesized for one lambda. Split from
    EmitCapture.cpp (source-size guard, zero behavior change); the
    display-instance machinery (construct/activate/carry/copy, cell
    access, captured reads) stays there.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>

namespace nlang {

//Frame prologue half 2: captured formals copy from their frame slots
//into the display fields (the analysis mirrored each capture onto the
//rebuilt formal). Flavor dispatch: a promoted formal wraps the raw
//argument into the box the field owns (the frame slot keeps the raw
//value, dead after the copy); a reference formal's pair is an 8-byte
//cell copy; a value formal goes through the struct-aware store (in
//place for struct-typed captures, born records from OP_New).
void VmBackend::EmitCapturedFormalCopies(SnFunction& func,
    BytecodeEmitter& emitter)
{
    for (auto& param : func.Params()) {
        if (!param.ContainFlags(NF_Captured))
            continue;
        EmitCapturedFormalCopy(param, emitter);
    }
}

//One captured formal's prologue copy (EmitCapturedFormalCopies' body —
//one flavor dispatch per parameter).
void VmBackend::EmitCapturedFormalCopy(SnFormalParam& param,
    BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(&param);
    auto* pDisplayClass = static_cast<SnClassDecl*>(
        pDisplayField->Parent());
    const uint16_t displaySlot = FindLocal(pDisplayClass->Name());
    const int fieldOff = FindClassFieldOffset(*pDisplayClass,
        pDisplayField->Name());
    NLANG_ASSERT(fieldOff >= 0);
    const uint16_t paramSlot = FindLocal(param.Name());
    if (param.ContainFlags(NF_Promoted)) {
        //VarLocal; Box; park the handle in the shared temp — no
        //nested emission between (the temps' two-op contract); the
        //raw value in the param slot rides its kind's heap GC arm
        //across the Box allocation (over-mark, same argument as
        //EmitPromotedValueWrap).
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(paramSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(
            param.EvalDataType()->Kind()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(m_currFunc->tempSlot);
    } else if (param.ContainFlags(NF_Ref)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(paramSlot);
    } else {
        EmitStructAwareStoreField(pDisplayField->EvalDataType(),
            displaySlot, static_cast<uint16_t>(fieldOff), paramSlot,
            emitter);
    }
}

//A captured catch variable's handler-entry copy: the runtime protocol
//lands the caught exception heap idx in the clause's frame slot, and
//every reference (captured or not) reads display storage once the
//descriptor is NF_Captured — so the first handler instruction bridges
//the slot into the display field. The value-flavor struct-aware store
//is the only arm: a catch variable is a class-typed value by
//construction (the flavor dispatch of the formal copy cannot apply).
void VmBackend::EmitCatchVarDisplayCopy(SnCatchClause& catchClause,
    uint16_t catchOff, BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(catchClause.VarLocal());
    auto* pDisplayClass = static_cast<SnClassDecl*>(
        pDisplayField->Parent());
    const uint16_t displaySlot = FindLocal(pDisplayClass->Name());
    const int fieldOff = FindClassFieldOffset(*pDisplayClass,
        pDisplayField->Name());
    NLANG_ASSERT(fieldOff >= 0);
    EmitStructAwareStoreField(pDisplayField->EvalDataType(),
        displaySlot, static_cast<uint16_t>(fieldOff), catchOff, emitter);
}

//Lambda literal: bind the anchor display instance (a frame local under
//the display class name — the anchor paragraph is always inside the
//literal's own emission frame) and the synthetic method into a delegate
//handle — the EmitMemberFuncHandleRef shape (receiver-first, pResult
//refresh, OP_MakeBoundFunc, assign).
void VmBackend::Access(SnLambdaExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto* pDisplay = expr.DisplayClass();
    auto* pMethod = expr.SyntheticMethod();
    NLANG_ASSERT(pDisplay && pDisplay->ContainFlags(NF_Generated)
        && pMethod && "lambda literal without its synthesis write-backs");
    const uint16_t displaySlot = FindLocal(pDisplay->Name());
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(displaySlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_MakeBoundFunc);
    emitter.EmitUint16(static_cast<uint16_t>(FunctionSlotFor(*pMethod)));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

} //namespace nlang
