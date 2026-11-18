/*---
    EmitStmtRef.cpp — v0.13.9 phase 3 Task 4: promoted/reference local
    storage emission. The promoted local/param family (stable boxed
    record per activation — the closure-cell representation) and the
    scalar/string reference dereference arms: decl-side box creation,
    the formal prologue, reads, stores, compound read-modify-write and
    the out-spill shapes. Split from EmitStmtAssign/EmitCall per the
    source-size guard (StatementResolverRef.cpp precedent).
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <cassert>

namespace nlang {

//Descriptor predicate (see the header comment): NF_Ref on the
//descriptor (propagated from the &-marked type node) plus a promotable
//declared kind — exactly the family the dereference ops serve.
bool VmBackend::IsDerefRefDescriptor(SnField* pField)
{
    if (!pField || !pField->ContainFlags(NF_Ref))
        return false;
    auto* pType = pField->EvalDataType();
    if (!pType)
        return false;
    const NodeKind kind = pType->Kind();
    return IsPrimitiveType(kind) || kind == NK_EnumDecl;
}

//Task 8: the T& formal test — NF_Ref with a type-parameter placeholder
//declared type (the placeholder is an NK_ClassDecl, so the deref-family
//test above never fires for it).
bool VmBackend::IsTypeArgRefFormal(SnField* pField)
{
    if (!pField || !pField->ContainFlags(NF_Ref))
        return false;
    auto* pType = pField->EvalDataType();
    if (!pType)
        return false;
    return TypeParamOrdinalOfType(*pType)
        != SnTypeNameExpr::kNoTypeParamOrdinal;
}

//Task 11: the cell-family test on the resolved TYPE node (IsDerefRef-
//Descriptor's kind arm without the flag part — the callers already
//know the binding is &). NK_String sits in the primitive block, so
//string elements take the cell family too.
uint8_t VmBackend::ByrefSlotKindOfType(SnField* evalType)
{
    const NodeKind elemKind = evalType ? evalType->Kind() : NK_Void;
    return IsPrimitiveType(elemKind) || elemKind == NK_EnumDecl
        ? static_cast<uint8_t>(RTK_ByrefCell)
        : static_cast<uint8_t>(RTK_Byref);
}

//Task 6: the value-side deref kind byte for a &-returning callee.
//kNoRefDeref (no normalization) for non-ref callees and for struct/
//class/array returns — the pair's low half is the record handle every
//value consumer (deep copy, member read) already reads. Scalar, string
//and enum returns name the payload tag OP_LoadRef derefs on any
//bindable pair (field cells, promoted boxes alike). Task 8: the
//sentinel is kNoRefDeref — RTK_Int32 is 0, so a 0 sentinel silently
//dropped the int/enum normalization.
uint8_t VmBackend::RefReturnDerefKind(SnFunction* pCallee) {
    if (!pCallee || !pCallee->ReturnType())
        return kNoRefDeref;
    auto* pRetType = pCallee->ReturnType();
    if (!pRetType->ContainFlags(NF_Ref))
        return kNoRefDeref;
    auto* pInner = pRetType->Field();
    if (!pInner)
        return kNoRefDeref;
    const NodeKind kind = pInner->Kind();
    if (kind != NK_String && kind != NK_EnumDecl
        && ScalarPrimIndexOf(kind) < 0)
        return kNoRefDeref;
    return BoxTypeTagOfKind(kind);
}

//See VmBackend.h — the slot holds a parked pair; the deref reads it
//back into itself.
void VmBackend::EmitRefDerefInPlace(uint8_t derefKind, uint16_t slot,
    BytecodeEmitter& emitter) {
    emitter.Emit(OpCode::OP_LoadRef);
    emitter.EmitByte(derefKind);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(slot);
}

//Activation-time box for one promoted declarator (design §3.2): the
//payload starts at the type default (zeroed accumulator → AllocBoxed-
//Value stores 0), the slot carries the box handle. The decomposed init
//assign (for an initialized declarator) overwrites the payload through
//the promoted store path.
void VmBackend::EmitPromotedDeclStorage(uint16_t offset, SnField* evalType,
    BytecodeEmitter& emitter)
{
    emitter.Emit(OpCode::OP_ConstZero);
    emitter.Emit(OpCode::OP_Box);
    emitter.EmitByte(BoxTypeTagOfKind(evalType->Kind()));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(offset);
}

//Promoted-formal prologue (see the header comment): the raw argument
//still sits in the frame slot when the body starts; wrap it into the
//stable box and rewrite the slot to the box handle.
//  VarLocal(slot); Box(tag); Assign(slot)
//GC safety: Box's allocation may collect — the raw value in the slot
//rides the RTK_Byref heap arm (over-mark only) and, for strings, the
//caller's Dual-valued staging slot keeps the handle rooted through the
//prologue. The stale upper half the Assign leaves is read by nobody:
//the field ops, RefOfPromoted and the GC band consume the low half
//only.
void VmBackend::EmitPromotedParamPrologue(SnFunction& func,
    BytecodeEmitter& emitter)
{
    for (auto& param : func.Params()) {
        if (!param.ContainFlags(NF_Promoted))
            continue;
        auto target = ResolveBareIdentifier(&param);
        NLANG_ASSERT(target.kind == BareIdTarget::Local);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(
            param.EvalDataType()->Kind()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(target.localOffset);
    }
}

//Identifier read arm for promoted/reference locals: emits the payload
//or dereference read and returns true; false leaves the caller on the
//plain 8-byte copy (aggregate-typed references — the pair slot reads
//as a handle slot to the field ops downstream).
bool VmBackend::EmitRefLocalRead(SnField* field, const BareIdTarget& target,
    uint16_t resultOffset, BytecodeEmitter& emitter)
{
    if (field->ContainFlags(NF_Promoted)) {
        //Promoted: read the box payload (byte offset = cell 1, payload
        //follows the tag cell; the 8-byte stride copy is width-correct
        //for narrow and wide payloads alike).
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(kHeapCellBytes);
        return true;
    }
    if (IsDerefRefDescriptor(field)) {
        //Reference to scalar/enum/string storage: dereference read.
        emitter.Emit(OpCode::OP_LoadRef);
        emitter.EmitByte(BoxTypeTagOfKind(
            field->EvalDataType()->Kind()));
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(target.localOffset);
        return true;
    }
    if (IsTypeArgRefFormal(field)) {
        //Task 8: reference to a generic formal's storage — the pair's
        //kind resolves at runtime (frame context's type argument):
        //scalar/string dereference, struct/identity raw pair copy (D9).
        emitter.Emit(OpCode::OP_LoadRef_T);
        emitter.EmitByte(TypeParamOrdinalOfType(
            *field->EvalDataType()));
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(target.localOffset);
        return true;
    }
    return false;
}

//Write-through store to a promoted local's box payload: stage the RHS,
//then StoreField into the payload cells (byte offset = cell 1, the tag
//cell precedes it).
void VmBackend::EmitPromotedAssignBareLocal(SnAssignStmt& assign,
    SnField* pLeftField, uint16_t offset, BytecodeEmitter& emitter)
{
    EvalAreaClaim claim(*this, 1);
    uint16_t valueSlot = claim.base();
    EmitExpression(*assign.Right(), emitter, valueSlot);
    //Scalars/strings never trip the rank check; kept for parity with
    //the plain local store.
    EmitArrayRankBoundary(pLeftField->EvalDataType(), assign.Right(),
        valueSlot, emitter);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(offset);
    emitter.EmitUint16(kHeapCellBytes);
    emitter.EmitUint16(valueSlot);
}

//Store through a reference local. The decomposed binding assign (the
//decl's own `r = init`, NF_Generated) points the pair at the target
//storage via the MakeRef family; user stores dereference.
void VmBackend::EmitRefAssignBareLocal(SnAssignStmt& assign,
    SnField* pLeftField, uint16_t offset, BytecodeEmitter& emitter)
{
    if (assign.ContainFlags(NF_Generated)) {
        EmitRefBindInit(assign, offset, emitter);
        return;
    }
    EvalAreaClaim claim(*this, 1);
    uint16_t valueSlot = claim.base();
    EmitExpression(*assign.Right(), emitter, valueSlot);
    EmitArrayRankBoundary(pLeftField->EvalDataType(), assign.Right(),
        valueSlot, emitter);
    emitter.Emit(OpCode::OP_StoreRef);
    emitter.EmitByte(BoxTypeTagOfKind(
        pLeftField->EvalDataType()->Kind()));
    emitter.EmitUint16(offset);
    emitter.EmitUint16(valueSlot);
}

//The `r = init` binding store. Task 5: the same MakeRef family the
//&-argument arm uses (spec §3.9 — one judgment, one emission).
void VmBackend::EmitRefBindInit(SnAssignStmt& assign, uint16_t offset,
    BytecodeEmitter& emitter)
{
    EmitMakeRef(offset, *assign.Right(), emitter);
}

//Task 8: user store through a T& formal in a generic body. The body is
//template-shared across instantiations, so no compile-time kind exists
//— OP_StoreRef_T resolves the pair's kind from the executing frame's
//instantiation descriptor (scalar/string cell write, struct in-place
//record overwrite, identity slot rebind per D9). The decomposed
//binding assign keeps EmitRefAssignBareLocal's MakeRef path above.
void VmBackend::EmitRefTypeArgAssignBareLocal(SnAssignStmt& assign,
    SnField* pLeftField, uint16_t offset, BytecodeEmitter& emitter)
{
    EvalAreaClaim claim(*this, 1);
    uint16_t valueSlot = claim.base();
    EmitExpression(*assign.Right(), emitter, valueSlot);
    EmitArrayRankBoundary(pLeftField->EvalDataType(), assign.Right(),
        valueSlot, emitter);
    emitter.Emit(OpCode::OP_StoreRef_T);
    emitter.EmitByte(TypeParamOrdinalOfType(
        *pLeftField->EvalDataType()));
    emitter.EmitUint16(offset);
    emitter.EmitUint16(valueSlot);
}

//Compound read-modify-write through promoted/reference local storage:
//old value and RHS park in an exclusive EvalAreaClaim(2) across the
//RHS emission (the same clobber discipline as the member arms). The
//payload read and the store mirror EmitRefLocalRead/EmitPromotedAssign-
//BareLocal's shapes over the compound result.
void VmBackend::EmitCompoundRefLocal(SnCompoundAssignStmt& ca,
    SnField* field, const BareIdTarget& target, int op,
    BytecodeEmitter& emitter)
{
    EvalAreaClaim claim(*this, 2);
    uint16_t oldValSlot = claim.base();
    uint16_t rhsSlot = claim.base() + kFrameSlotBytes;
    const bool isTypeArgRef = IsTypeArgRefFormal(field);
    if (field->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(oldValSlot);
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(kHeapCellBytes);
    } else if (isTypeArgRef) {
        //Task 8: the T& formal's old value — descriptor-dispatched read.
        emitter.Emit(OpCode::OP_LoadRef_T);
        emitter.EmitByte(TypeParamOrdinalOfType(
            *field->EvalDataType()));
        emitter.EmitUint16(oldValSlot);
        emitter.EmitUint16(target.localOffset);
    } else {
        emitter.Emit(OpCode::OP_LoadRef);
        emitter.EmitByte(BoxTypeTagOfKind(
            field->EvalDataType()->Kind()));
        emitter.EmitUint16(oldValSlot);
        emitter.EmitUint16(target.localOffset);
    }
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot,
        field->EvalDataType());
    if (field->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(kHeapCellBytes);
        emitter.EmitUint16(oldValSlot);
    } else if (isTypeArgRef) {
        emitter.Emit(OpCode::OP_StoreRef_T);
        emitter.EmitByte(TypeParamOrdinalOfType(
            *field->EvalDataType()));
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(oldValSlot);
    } else {
        emitter.Emit(OpCode::OP_StoreRef);
        emitter.EmitByte(BoxTypeTagOfKind(
            field->EvalDataType()->Kind()));
        emitter.EmitUint16(target.localOffset);
        emitter.EmitUint16(oldValSlot);
    }
}

//Out-spill shape selector (v0.13.9 phase 3 Task 4): a promoted target
//spills into its box payload, a dereference reference spills through
//its pair with the value tag, anything else keeps the plain 8-byte
//assign.
uint8_t VmBackend::OutSpillRefKindOf(SnField* pField)
{
    if (pField->ContainFlags(NF_Promoted))
        return RTK_Byref;
    if (IsDerefRefDescriptor(pField))
        return BoxTypeTagOfKind(pField->EvalDataType()->Kind());
    return 0;
}

void VmBackend::EmitOutSpills(const std::vector<OutSpill>& spills,
                              BytecodeEmitter& emitter)
{
    for (const auto& s : spills) {
        const uint16_t callSlot = static_cast<uint16_t>(
            m_currFunc->callParamBase + s.slotIdx * kFrameSlotBytes);
        if (s.refKind == RTK_Byref) {
            //Promoted target: spill into the box payload cells (byte
            //offset = cell 1).
            emitter.Emit(OpCode::OP_StoreField);
            emitter.EmitUint16(s.localOffset);
            emitter.EmitUint16(kHeapCellBytes);
            emitter.EmitUint16(callSlot);
        } else if (s.refKind != 0) {
            //Through-reference target: the tagged StoreRef.
            emitter.Emit(OpCode::OP_StoreRef);
            emitter.EmitByte(s.refKind);
            emitter.EmitUint16(s.localOffset);
            emitter.EmitUint16(callSlot);
        } else {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(callSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(s.localOffset);
        }
    }
}

} //namespace nlang
