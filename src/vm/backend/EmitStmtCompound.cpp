/*---
    EmitStmtCompound.cpp — 复合赋值语句发射（x op= v 目标形态族）。
    从 EmitStmtAssign.cpp 拆出（2026-10-10，source_size_guard 收口：
    按语句族分文件，函数尺寸保持 50 行内）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/NodeConsts.h>

namespace nlang {

//Compound assignment: x += y, this.f -= 1, obj.f *= 2, arr[i] %= 2.
//Phase 9a: left-value is evaluated only once (read-modify-write).
//0.8.4: bare-subscript LHS (arr[i] += v) — base and index each
//evaluated exactly once, parked in an exclusive claim.
void VmBackend::Access(SnCompoundAssignStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    auto& ca = static_cast<SnCompoundAssignStmt&>(stmt);
    auto op = ca.Op();
    if (ca.Left()->Kind() == NK_IdentifierExpr) {
        auto& idExpr = static_cast<SnIdentifierExpr&>(*ca.Left());
        EmitCompoundAssignBareIdentifier(ca, idExpr.Field(), op,
            emitter);
    } else if (ca.Left()->Kind() == NK_MemberExpr) {
        EmitCompoundAssignMemberField(ca,
            static_cast<SnMemberExpr&>(*ca.Left()), op, emitter);
    } else if (ca.Left()->Kind() == NK_SubscriptExpr) {
        EmitCompoundAssignSubscript(ca,
            static_cast<SnSubscriptExpr&>(*ca.Left()), op, emitter);
    }
    return;
}

//Bare-identifier compound assign: local slot in-place, or implicit
//this.<field> read-modify-write.
void VmBackend::EmitCompoundAssignBareIdentifier(SnCompoundAssignStmt& ca,
        SnField* field, int op, BytecodeEmitter& emitter) {
    if (!field) {
        //Round-12: the resolver should always bind the field for a
        //compound assignment. If it didn't, that's an internal error.
        throw std::runtime_error(
            "NLang backend: compound assignment with unbound field");
    }
    BareIdTarget target = ResolveBareIdentifier(field);
    if (target.kind == BareIdTarget::Local) {
        //Local variable: compute in-place on the local slot
        EmitExpression(*ca.Right(), emitter, m_currFunc->tempSlot2);
        EmitCompoundOp(op, emitter, target.localOffset,
            m_currFunc->tempSlot2, field->EvalDataType());
    } else if (target.kind == BareIdTarget::ThisField) {
        EmitCompoundAssignThisField(ca, field, target.fieldOff, op,
            emitter);
    }
}

//Implicit this.<field> compound assign: same read-modify-write shape
//as the explicit MemberExpr path, with the receiver sourced from
//ImplicitThisSlot(). Phase 10 audit round-3: all three live values
//(receiver, old field value, RHS) park in exclusive EvalAreaClaim
//slots. Parking the receiver in tempSlot — unavoidable across the RHS
//emission in a read-modify-write — let any nested RHS expression
//clobber it, because PickTempSlot falls back to tempSlot whenever its
//exclude slot is not one of the four temps (claim slots included).
void VmBackend::EmitCompoundAssignThisField(SnCompoundAssignStmt& ca,
        SnField* field, int fieldOff, int op, BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    uint16_t oldValSlot = claimBase + kFrameSlotBytes;
    uint16_t rhsSlot = claimBase + 2 * kFrameSlotBytes;
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(ImplicitThisSlot());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(oldValSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot,
        field->EvalDataType());
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
    emitter.EmitUint16(oldValSlot);
}

//Member compound assign (obj.f op= v). Phase 10 audit round-3:
//read-modify-write parks three live values across the RHS emission
//(receiver, old value, RHS) — all three go into exclusive
//EvalAreaClaim slots for the same reason as the implicit this-field
//path: PickTempSlot falls back to tempSlot for non-temp exclude
//slots, so a receiver parked in any temp is clobberable by a nested
//RHS.
void VmBackend::EmitCompoundAssignMemberField(SnCompoundAssignStmt& ca,
        SnMemberExpr& memberExpr, int op, BytecodeEmitter& emitter) {
    auto* outerType = memberExpr.Outer()->EvalDataType();
    if (!outerType) return;
    auto* inner = memberExpr.Inner();
    if (inner->Kind() != NK_IdentifierExpr) return;
    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
    SnField* lhsFieldType = memberExpr.EvalDataType();
    //Round-12: unexpected outer type for member assignment.
    if (outerType->Kind() != NK_ClassDecl
        && outerType->Kind() != NK_StructDecl)
    {
        throw std::runtime_error(
            "NLang backend: member assignment with unexpected outer type");
    }
    uint16_t fieldOff = ResolveMemberFieldOffset(outerType, fieldName);
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    uint16_t oldValSlot = claimBase + kFrameSlotBytes;
    uint16_t rhsSlot = claimBase + 2 * kFrameSlotBytes;
    EmitExpression(*memberExpr.Outer(), emitter, claimBase);
    if (outerType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(claimBase);
    }
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(oldValSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(fieldOff);
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot, lhsFieldType);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(oldValSlot);
}

//Bare-subscript compound assign (0.8.4): arr[i] op= v / li[i] op= v /
//d[k] op= v. JLS 15.26.1 read-modify-write: base and index are each
//evaluated exactly once — all four live values (base, index, old
//element, RHS) park in an exclusive EvalAreaClaim(4) across the RHS
//emission and the store, the same discipline as the member/this-field
//paths above (PickTempSlot falls back to tempSlot for non-temp exclude
//slots, so a parked value in any temp is clobberable by a nested RHS).
void VmBackend::EmitCompoundAssignSubscript(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, int op, BytecodeEmitter& emitter) {
    //Element type: the resolver stamps the subscript's EvalDataType as
    //the element (it peels any array token; BindArrayTypeToken is a
    //no-op for SubscriptExpr), so no token peel is needed here.
    auto* elemType = sub.EvalDataType();
    if (!elemType) return;
    EvalAreaClaim claim(*this, 4);
    uint16_t claimBase = claim.base();
    uint16_t indexSlot = claimBase + kFrameSlotBytes;
    uint16_t oldValSlot = claimBase + 2 * kFrameSlotBytes;
    uint16_t rhsSlot = claimBase + 3 * kFrameSlotBytes;
    if (IsContainerSubscript(*sub.Array())) {
        EmitCompoundSubscriptContainer(ca, sub, elemType, op,
            claimBase, indexSlot, oldValSlot, rhsSlot, emitter);
    } else {
        EmitCompoundSubscriptArray(ca, sub, elemType, op,
            claimBase, indexSlot, oldValSlot, rhsSlot, emitter);
    }
}

//0.8.4: container-base compound subscript (li[i] op= v / d[k] op= v) —
//the subscript is sugar over get()/set() on the parked [base, index,
//result] claim slots, so base and index are NOT re-emitted (JLS
//15.26.1 single evaluation). The Dict key is boxed once in place — the
//same boxed value feeds both calls.
void VmBackend::EmitCompoundSubscriptContainer(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, SnField* elemType, int op,
        uint16_t claimBase, uint16_t indexSlot, uint16_t oldValSlot,
        uint16_t rhsSlot, BytecodeEmitter& emitter) {
    auto* pGenClass = static_cast<SnClassDecl*>(
        sub.Array()->EvalDataType());
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    bool isList = (baseName == kBuiltinListTypeName
        && !typeArgs.empty());
    bool isDict = (baseName == kBuiltinDictTypeName
        && typeArgs.size() > 1);
    //Array-typed slots are interned tokens — raw handles, no box
    //(BoxingTagFor default). Dict keys box when primitive; List's
    //index is int.
    auto keyBox = isDict
        ? BoxingTagFor(typeArgs[0])
        : BoxingTagResult{0, false};
    SnField* pElem = isList ? typeArgs[0]
        : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
    auto valBox = BoxingTagFor(pElem);
    //1. Base → claim[0], null-checked (receiver first, JLS 15.26.1).
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //2. Index → claim[1]; a Dict key is boxed once in place.
    EmitExpression(*sub.Index(), emitter, indexSlot);
    if (keyBox.isPrimitive) {
        EmitPResultRefresh(emitter, indexSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(keyBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(indexSlot);
    }
    //3. Old element: get() on the parked [base, index] → claim[2],
    //unboxed for primitive T/V.
    EmitSubscriptGetFromClaim(claimBase, indexSlot, oldValSlot,
        valBox, emitter);
    //4. RHS → claim[3], raw — the compound op reads the slots;
    //boxing is a store-side concern for the set() below.
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    //5. oldValSlot = oldValSlot op rhsSlot.
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot, elemType);
    //6. Store: set() on [base, index, result].
    EmitSubscriptSetFromClaim(claimBase, indexSlot, oldValSlot,
        valBox, emitter);
}

//0.8.4: get() call over the already-parked [base, index] claim slots —
//mirrors EmitContainerGetCall's unbox discipline but without re-emitting
//base/index (they were evaluated once for the read-modify-write).
void VmBackend::EmitSubscriptGetFromClaim(uint16_t claimBase,
        uint16_t indexSlot, uint16_t oldValSlot, BoxingTagResult valBox,
        BytecodeEmitter& emitter) {
    CopyClaimToCallParams(claimBase, 2, emitter);
    uint16_t nameIdx = AddStringConstant("get");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    if (valBox.isPrimitive) {
        emitter.Emit(OpCode::OP_Unbox);
        emitter.EmitByte(valBox.tag);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(oldValSlot);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//0.8.4: set() call over the parked [base, index, result] claim slots —
//box the result in place when T/V is primitive (EmitCompoundOp's
//numeric path leaves pResult stale, so refresh before OP_Box).
void VmBackend::EmitSubscriptSetFromClaim(uint16_t claimBase,
        uint16_t indexSlot, uint16_t oldValSlot, BoxingTagResult valBox,
        BytecodeEmitter& emitter) {
    if (valBox.isPrimitive) {
        EmitPResultRefresh(emitter, oldValSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(valBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(oldValSlot);
    }
    CopyClaimToCallParams(claimBase, 3, emitter);
    uint16_t nameIdx = AddStringConstant("set");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//0.8.4: array-base compound subscript (arr[i] op= v) — direct element
//ops, no call frames, so no bulk copy.
void VmBackend::EmitCompoundSubscriptArray(SnCompoundAssignStmt& ca,
        SnSubscriptExpr& sub, SnField* elemType, int op,
        uint16_t claimBase, uint16_t indexSlot, uint16_t oldValSlot,
        uint16_t rhsSlot, BytecodeEmitter& emitter) {
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    EmitExpression(*sub.Index(), emitter, indexSlot);
    //No copy on read (Phase 9d-3: value copies belong at store
    //boundaries): claim[2] aliases the element until the computed
    //result is stored.
    emitter.Emit(OpCode::OP_LoadElement);
    emitter.EmitUint16(oldValSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(indexSlot);
    EmitExpression(*ca.Right(), emitter, rhsSlot);
    EmitCompoundOp(op, emitter, oldValSlot, rhsSlot, elemType);
    //Struct deep-copy of the computed value before the store
    //(value-semantics boundary, mirrors EmitArrayElementStore).
    if (RuntimeTypeKind(elemType) == RTK_Struct
        && elemType->Kind() == NK_StructDecl) {
        EmitStructDeepCopy(oldValSlot, oldValSlot,
            static_cast<SnStructDecl&>(*elemType), emitter);
    }
    emitter.Emit(OpCode::OP_StoreElement);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(indexSlot);
    emitter.EmitUint16(oldValSlot);
}

} //namespace nlang
