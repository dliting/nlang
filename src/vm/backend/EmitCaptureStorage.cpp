/*---
    EmitCaptureStorage.cpp — 0.13.10 Task 6: lambda capture emission,
    captured-storage mutation half. The captured-storage plain assign,
    the compound read-modify-write, the reference-binding MakeRef arm,
    and the captured declarator's storage redirect. The frame machinery
    (prologue, receiver redirect, cell chase, reads) lives in
    EmitCapture.cpp; the analysis (builder/LambdaCaptureAnalysis.cpp)
    synthesized the display classes and moved the bodies.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <stdexcept>

namespace nlang {

//Reference-typed capture store-through: re-stage the pair out of the
//field cell into the scratch record slot (the chased record there is
//dead by then — the in-place LoadField overwrites it with the pair),
//then the tagged store-through writes valueSlot via the (base, cell)
//pair. Shared by the plain assign and the compound store-back so the
//pair re-staging cannot drift: the compound's staged pair is consumed
//by its in-place deref, while the local analog keeps its pair in the
//persistent local slot and never re-stages.
void VmBackend::EmitCapturedRefStore(SnClassField& displayField,
    int fieldOff, uint16_t recordSlot, uint16_t valueSlot,
    BytecodeEmitter& emitter)
{
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(recordSlot);   //the pair re-staged here
    emitter.EmitUint16(recordSlot);
    emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
    if (IsTypeArgRefFormal(&displayField)) {
        emitter.Emit(OpCode::OP_StoreRef_T);
        emitter.EmitByte(
            TypeParamOrdinalOfType(*displayField.EvalDataType()));
    } else {
        emitter.Emit(OpCode::OP_StoreRef);
        emitter.EmitByte(
            BoxTypeTagOfKind(displayField.EvalDataType()->Kind()));
    }
    emitter.EmitUint16(recordSlot);
    emitter.EmitUint16(valueSlot);
}

//Plain assign through captured storage. Flavor dispatch mirrors the
//local promoted/ref/value assign shapes re-hosted over a chased cell:
//the NF_Generated binding init stages the pair via the MakeRef family
//and publishes it into the field cell; a promoted store writes the box
//payload; a deref-flavor user store goes through the tagged
//store-through (EmitCapturedRefStore); a value store goes through the
//struct-aware cell store. Aggregate-typed references keep the value
//shape (their field cell reads as a handle slot — the local path's
//plain-form choice).
void VmBackend::EmitCapturedAssign(SnAssignStmt& assign, SnField* field,
    const BareIdTarget& target, BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(field);
    EvalAreaClaim claim(*this, 2);
    uint16_t cellSlot = claim.base();
    uint16_t valueSlot = cellSlot + kFrameSlotBytes;
    if (pDisplayField->ContainFlags(NF_Ref)
        && assign.ContainFlags(NF_Generated)) {
        //Binding init (`int& r = expr;` decomposed): the pair lands in
        //the field cell.
        EmitMakeRef(valueSlot, *assign.Right(), emitter);
        EmitCaptureCellLoad(cellSlot, target, pDisplayField, emitter);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        emitter.EmitUint16(valueSlot);
        return;
    }
    EmitExpression(*assign.Right(), emitter, valueSlot);
    EmitArrayRankBoundary(pDisplayField->EvalDataType(), assign.Right(),
        valueSlot, emitter);
    EmitCaptureCellLoad(cellSlot, target, pDisplayField, emitter);
    if (pDisplayField->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(kHeapCellBytes);
        emitter.EmitUint16(valueSlot);
        return;
    }
    if (IsDerefRefDescriptor(pDisplayField)
        || IsTypeArgRefFormal(pDisplayField)) {
        EmitCapturedRefStore(*pDisplayField, target.fieldOff, cellSlot,
            valueSlot, emitter);
        return;
    }
    //Value capture.
    EmitCaptureCellStore(pDisplayField, target.fieldOff, cellSlot,
        valueSlot, emitter);
}

//Old-value read of a captured compound: the chased record sits in
//cellSlot; the flavor dispatch stages the old value (the deref flavors
//in place over the staged pair — dst == src, the EmitRefDerefInPlace
//contract; the local analog reads its persistent pair slot instead).
void VmBackend::EmitCapturedOldValueRead(SnClassField* pDisplayField,
    const BareIdTarget& target, uint16_t cellSlot, uint16_t oldValSlot,
    BytecodeEmitter& emitter)
{
    if (pDisplayField->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(oldValSlot);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(kHeapCellBytes);
        return;
    }
    if (pDisplayField->ContainFlags(NF_Ref)
        || IsTypeArgRefFormal(pDisplayField)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(oldValSlot);   //the pair staged here
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        if (IsTypeArgRefFormal(pDisplayField)) {
            emitter.Emit(OpCode::OP_LoadRef_T);
            emitter.EmitByte(TypeParamOrdinalOfType(
                *pDisplayField->EvalDataType()));
        } else {
            emitter.Emit(OpCode::OP_LoadRef);
            emitter.EmitByte(BoxTypeTagOfKind(
                pDisplayField->EvalDataType()->Kind()));
        }
        emitter.EmitUint16(oldValSlot);
        emitter.EmitUint16(oldValSlot);
        return;
    }
    //Value capture: the pair-free field read.
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(oldValSlot);
    emitter.EmitUint16(cellSlot);
    emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
}

//Compound read-modify-write through captured storage: [chased record,
//old value, RHS] in an exclusive EvalAreaClaim(3) — the walker's
//uniform compound charge, so no walker change rides on this path. The
//dereference flavors stage the pair into the old-value slot and deref
//in place (dst == src); the store-back RE-STAGES the pair from the
//field cell before the store-through — the in-place deref consumed the
//staged copy, and OP_StoreRef reads the pair from its first operand
//slot.
void VmBackend::EmitCapturedCompound(SnCompoundAssignStmt& ca,
    SnField* field, const BareIdTarget& target, int op,
    BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(field);
    EvalAreaClaim claim(*this, 3);
    uint16_t cellSlot = claim.base();
    uint16_t oldValSlot = cellSlot + kFrameSlotBytes;
    uint16_t rhsSlot = oldValSlot + kFrameSlotBytes;
    EmitCaptureCellLoad(cellSlot, target, pDisplayField, emitter);
    EmitCapturedOldValueRead(pDisplayField, target, cellSlot,
        oldValSlot, emitter);
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot,
        pDisplayField->EvalDataType());
    if (pDisplayField->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(kHeapCellBytes);
        emitter.EmitUint16(oldValSlot);
    } else if (pDisplayField->ContainFlags(NF_Ref)
        || IsTypeArgRefFormal(pDisplayField)) {
        EmitCapturedRefStore(*pDisplayField, target.fieldOff, cellSlot,
            oldValSlot, emitter);
    } else {
        //Value capture (struct compound targets are rejected by the
        //resolver gate — the plain cell store is the only shape here).
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        emitter.EmitUint16(oldValSlot);
    }
}

//Reference-binding over chased display storage: the DisplayField /
//CaptureField arm of EmitMakeRefOfIdentifier. The pair builds over the
//chased storage base; flavor flags ride the captured descriptor
//itself (mirrored onto the display field by the analysis).
void VmBackend::EmitCapturedMakeRef(uint16_t dstOffset,
    SnIdentifierExpr& idExpr, const BareIdTarget& target,
    BytecodeEmitter& emitter)
{
    auto* pTarget = idExpr.Field();
    if (pTarget->ContainFlags(NF_Ref)) {
        //Reference capture: the field cell already holds the pair —
        //a raw 8-byte copy preserves the alias (pass-through).
        EvalAreaClaim claim(*this, 1);
        uint16_t cellSlot = claim.base();
        EmitCaptureCellLoad(cellSlot, target, pTarget, emitter);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        return;
    }
    if (pTarget->ContainFlags(NF_Promoted)) {
        //Promoted capture: the chase already staged the box handle —
        //the payload pair comes straight off it.
        EvalAreaClaim claim(*this, 1);
        uint16_t cellSlot = claim.base();
        EmitCaptureCellLoad(cellSlot, target, pTarget, emitter);
        emitter.Emit(OpCode::OP_RefOfPromoted);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(cellSlot);
        return;
    }
    auto* pType = pTarget->EvalDataType();
    NLANG_ASSERT(pType != nullptr);
    if (IsPrimitiveType(pType->Kind()) || pType->Kind() == NK_EnumDecl)
        throw std::runtime_error(
            "NLang backend: scalar reference argument on an "
            "un-promoted capture (promotion marking missed): " +
            idExpr.Name() + " in " + m_currFunc->func->name);
    //Struct/class/array value capture: the field-bind throat pairs the
    //record the cell names (or the handle — equivalent notation).
    //base == scratch is legal: the Record arm's LoadField reads before
    //it writes.
    EvalAreaClaim claim(*this, 1);
    uint16_t cellSlot = claim.base();
    EmitCaptureCellLoad(cellSlot, target, pTarget, emitter);
    EmitFieldRefBind(pTarget->EvalDataType(), dstOffset, cellSlot,
        target.fieldOff / kHeapCellBytes, cellSlot, emitter);
}

//A captured declarator owns no frame slot — the display field IS the
//storage. The prologue already built the display instance; a promoted
//capture needs only its activation box written into the field cell
//(the payload starts at the type default, same as EmitPromotedDecl-
//Storage), and a reference/value capture's cell is established by the
//decomposed init assign that follows. True when the declarator was
//captured (the caller skips its slot allocation).
bool VmBackend::EmitCapturedDeclaratorStorage(SnField* pDeclNode,
    SnField* evalType, BytecodeEmitter& emitter)
{
    if (!pDeclNode || !pDeclNode->ContainFlags(NF_Captured))
        return false;
    auto* pDisplayField = CapturedDisplayFieldOf(pDeclNode);
    NLANG_ASSERT(pDisplayField
        && "captured declarator without its display field");
    if (pDisplayField->ContainFlags(NF_Promoted)) {
        auto target = DisplayFieldOfCaptured(pDeclNode);
        NLANG_ASSERT(target.kind == BareIdTarget::DisplayField);
        emitter.Emit(OpCode::OP_ConstZero);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(evalType->Kind()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        emitter.EmitUint16(m_currFunc->tempSlot);
    }
    return true;
}

} //namespace nlang
