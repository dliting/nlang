/*---
    EmitStmtAssign.cpp — 赋值语句发射：赋值目标形态族与复合赋值。
    从 EmitStmtDecl.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
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
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes
    //Assignment statement.
void VmBackend::Access(SnAssignStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& assign = static_cast<SnAssignStmt&>(stmt);
        if (assign.Left()->Kind() == NK_IdentifierExpr) {
            auto& idExpr = static_cast<SnIdentifierExpr&>(*assign.Left());
            auto* field = idExpr.Field();
            if (field) {
                EmitAssignBareIdentifier(assign, field, emitter);
            }
        } else if (assign.Left()->Kind() == NK_MemberExpr) {
            //Struct or class field assignment
            EmitAssignMemberField(assign,
                static_cast<SnMemberExpr&>(*assign.Left()), emitter);
        }
        return;
}

//Bare-identifier lvalue: local slot write (struct deep-copy variant
//included) or implicit this.<field> write inside a method.
void VmBackend::EmitAssignBareIdentifier(SnAssignStmt& assign,
        SnField* field, BytecodeEmitter& emitter) {
    auto target = ResolveBareIdentifier(field);
    if (target.kind == BareIdTarget::Local) {
        EmitAssignToLocal(assign, target.localOffset,
            field->EvalDataType(), emitter);
    } else if (target.kind == BareIdTarget::ThisField) {
        EmitAssignToThisField(assign, target.fieldOff, emitter);
    }
}

//Direct local write. Array locals (`Point[] arr = ...`) carry their
//interned array token in EvalDataType (0.7.3 B), which RuntimeTypeKind
//files as RTK_Array — so the struct deep-copy branch no longer needs a
//separate IsArrayType guard, and array assignment stays a reference
//(heap idx) copy.
void VmBackend::EmitAssignToLocal(SnAssignStmt& assign, uint16_t offset,
        SnField* varType, BytecodeEmitter& emitter) {
    if (varType && RuntimeTypeKind(varType) == RTK_Struct
        && varType->Kind() == NK_StructDecl) {
        //Struct assignment: evaluate right to temp, then deep-copy.
        //Per-unit: cross-unit structs slot as import placeholders.
        EmitExpression(*assign.Right(), emitter, m_currFunc->tempSlot2);
        EmitStructDeepCopy(offset, m_currFunc->tempSlot2,
            static_cast<SnStructDecl&>(*varType), emitter);
    } else {
        //Phase 10 audit round-7: stage the RHS in an EvalAreaClaim(1)
        //and copy to the destination — emitting straight into `offset`
        //let a binary's LEFT operand write the destination before the
        //RIGHT evaluated (`k = li[0] + li[k]` read the clobbered slot:
        //OOB or a silently wrong index). JLS 15.26.1: the destination is
        //written only after the whole RHS has been evaluated.
        EvalAreaClaim claim(*this, 1);
        uint16_t valueSlot = claim.base();
        EmitExpression(*assign.Right(), emitter, valueSlot);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(valueSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(offset);
    }
}

//Implicit this.<field> = value (bare member write inside a method).
//Same opcode shape as the MemberExpr write.
void VmBackend::EmitAssignToThisField(SnAssignStmt& assign, int fieldOff,
        BytecodeEmitter& emitter) {
    EmitExpression(*assign.Right(), emitter,
        m_currFunc->tempSlot2);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(ImplicitThisSlot());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
    emitter.EmitUint16(m_currFunc->tempSlot2);
}

//Member lvalue: arr[i].field / li[i].field subscripted-element stores
//first (they fully consume the statement), then plain class/struct
//field stores.
void VmBackend::EmitAssignMemberField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, BytecodeEmitter& emitter) {
    if (EmitAssignSubscriptElementStore(assign, memberExpr, emitter))
        return;
    auto* outerType = memberExpr.Outer()->EvalDataType();
    if (outerType && outerType->Kind() == NK_ClassDecl) {
        EmitAssignClassField(assign, memberExpr,
            static_cast<SnClassDecl*>(outerType), emitter);
    } else if (outerType && outerType->Kind() == NK_StructDecl) {
        EmitAssignStructField(assign, memberExpr,
            static_cast<SnStructDecl*>(outerType), emitter);
    }
}

//Field offset on a class/struct element type for the subscripted-member
//stores; throws when the field is not found.
uint16_t VmBackend::ResolveMemberFieldOffset(SyntaxNode* elemType,
        const std::string& fieldName) {
    if (elemType->Kind() == NK_ClassDecl) {
        int off = FindClassFieldOffset(
            *static_cast<SnClassDecl*>(elemType), fieldName);
        if (off < 0) {
            throw std::runtime_error(
                "NLang backend: class field offset not found: "
                + fieldName);
        }
        return static_cast<uint16_t>(off);
    }
    int off = FindFieldOffset(
        *static_cast<SnStructDecl*>(elemType), fieldName);
    if (off < 0) {
        throw std::runtime_error(
            "NLang backend: struct field offset not found: "
            + fieldName);
    }
    return static_cast<uint16_t>(off);
}

//arr[i].field = v / li[i].field = v. The subscript outer's index must
//NOT alias tempSlot2 (which holds the right side value). Returns true
//when a class/struct element shape consumed the statement; false lets
//the caller fall through to plain member stores.
bool VmBackend::EmitAssignSubscriptElementStore(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, BytecodeEmitter& emitter) {
    if (memberExpr.Outer()->Kind() != NK_SubscriptExpr)
        return false;
    auto& sub = static_cast<SnSubscriptExpr&>(
        *memberExpr.Outer());
    auto* elemType = sub.EvalDataType();
    if (!(elemType
        && (elemType->Kind() == NK_ClassDecl
            || elemType->Kind() == NK_StructDecl)))
    {
        return false;
    }
    auto* inner = memberExpr.Inner();
    if (inner->Kind() != NK_IdentifierExpr) return true;
    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
    uint16_t fieldOff = ResolveMemberFieldOffset(elemType, fieldName);
    if (IsContainerSubscript(*sub.Array())) {
        EmitAssignContainerElementFieldStore(assign, sub, elemType,
            fieldOff, emitter);
        return true;
    }
    EmitAssignArrayElementFieldStore(assign, sub, elemType, fieldOff,
        emitter);
    return true;
}

//List/Dict receiver: `li[i].field = v` — the subscript is sugar over
//get(), so delegate to the generic NK_SubscriptExpr lowering
//(EvalAreaClaim + get() call). Phase 10 audit round-4: park [receiver,
//value] in an EvalAreaClaim(2) — the old tempSlot/tempSlot2 staging was
//clobbered by a call in the value (argument binaries fall through
//PickTempSlot to tempSlot; struct-argument deep copy scratches
//tempSlot), crashing or storing through the wrong object. Receiver
//first (JLS 15.26.1).
void VmBackend::EmitAssignContainerElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 2);
    uint16_t objSlot = claim.base();
    uint16_t valueSlot = objSlot + VALUE_SIZE;
    EmitExpression(sub, emitter, objSlot);
    if (elemType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(objSlot);
    }
    EmitExpression(*assign.Right(), emitter, valueSlot);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(objSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(valueSlot);
}

//Array receiver: `arr[i].field = v`. Phase 10 audit round-3: stage
//value/index/array in an exclusive EvalAreaClaim(3) — the old
//tempSlot/tempSlot2/callParamBase staging let a nested index expression
//(subscript-get, binary arithmetic — both scratch tempSlot via the
//PickTempSlot fall-through) clobber the parked array, crashing or
//silently storing through the wrong heap object. Same discipline as the
//container path above and SubscriptAssignStmt.
void VmBackend::EmitAssignArrayElementFieldStore(SnAssignStmt& assign,
        SnSubscriptExpr& sub, SyntaxNode* elemType, uint16_t fieldOff,
        BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    uint16_t indexSlot = claimBase + VALUE_SIZE;
    uint16_t valueSlot = claimBase + 2 * VALUE_SIZE;
    //1. Evaluate right side → claim[2]
    EmitExpression(*assign.Right(), emitter, valueSlot);
    //2. Evaluate index → claim[1]
    EmitExpression(*sub.Index(), emitter, indexSlot);
    //3. Evaluate array ref → claim[0], null-checked
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //4. load element into tempSlot (free scratch — all operands are
    //parked in exclusive claim slots now)
    emitter.Emit(OpCode::OP_LoadElement);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(indexSlot);
    //5. null_check the element (class) or skip (struct is value)
    if (elemType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(m_currFunc->tempSlot);
    }
    //6. store_field obj=tempSlot off=fieldOff src=claim[2]
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(valueSlot);
}

//Class field assignment: reference semantics, no deep copy. Phase 10
//audit round-4: park [receiver, value] in an EvalAreaClaim(2) — the old
//tempSlot/tempSlot2 staging was clobbered by a call in the RHS (argument
//binaries fall through PickTempSlot to tempSlot; struct-argument deep
//copy scratches tempSlot), crashing or storing through the wrong
//object. Receiver first (JLS 15.26.1).
void VmBackend::EmitAssignClassField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, SnClassDecl* classDecl,
        BytecodeEmitter& emitter) {
    auto* inner = memberExpr.Inner();
    if (inner->Kind() != NK_IdentifierExpr) return;
    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
    int fieldOffInt = FindClassFieldOffset(*classDecl, fieldName);
    if (fieldOffInt < 0) return;
    uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
    EvalAreaClaim claim(*this, 2);
    uint16_t objSlot = claim.base();
    uint16_t valueSlot = objSlot + VALUE_SIZE;
    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(objSlot);
    EmitExpression(*assign.Right(), emitter, valueSlot);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(objSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(valueSlot);
}

//Struct field assignment: resolves the field's offset and type, then
//delegates to the deep-copy or plain store. An array-typed field
//carries its interned array token in EvalDataType (0.7.3 B), which
//RuntimeTypeKind files as RTK_Array — array stores stay reference
//(heap idx) copies, with no separate IsArrayType guard.
void VmBackend::EmitAssignStructField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, SnStructDecl* structDecl,
        BytecodeEmitter& emitter) {
    auto* inner = memberExpr.Inner();
    if (inner->Kind() != NK_IdentifierExpr) return;
    auto fieldName = static_cast<SnIdentifierExpr*>(inner)->Name();
    int fieldOffInt = FindFieldOffset(*structDecl, fieldName);
    if (fieldOffInt < 0) return;
    uint16_t fieldOff = static_cast<uint16_t>(fieldOffInt);
    SnField* fieldType = nullptr;
    for (auto& sf : structDecl->Members()) {
        if (sf.Name() == fieldName) {
            fieldType = sf.EvalDataType();
            break;
        }
    }
    if (fieldType && RuntimeTypeKind(fieldType) == RTK_Struct) {
        EmitAssignStructFieldDeepCopy(assign, memberExpr, fieldOff,
            fieldType, emitter);
    } else {
        EmitAssignStructFieldPlain(assign, memberExpr, fieldOff,
            emitter);
    }
}

//Struct-to-struct field assignment: deep-copy first. Phase 10 audit
//round-4: stage rhs/copy/outer in an EvalAreaClaim(3) — same parking
//discipline as the other member targets (temps are pure scratch; the
//fresh-copy handle must survive the outer emission).
void VmBackend::EmitAssignStructFieldDeepCopy(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, uint16_t fieldOff, SnField* fieldType,
        BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 3);
    uint16_t copySlot = claim.base();
    uint16_t objSlot = copySlot + VALUE_SIZE;
    uint16_t rhsSlot = objSlot + VALUE_SIZE;
    EmitExpression(*assign.Right(), emitter, rhsSlot);
    //fieldType is the resolver-bound field-type node, so the slot
    //helper spells the same key the registration writer stored.
    EmitStructDeepCopy(copySlot, rhsSlot,
        static_cast<SnStructDecl&>(*fieldType), emitter);
    //Evaluate outer (parent struct's heap index)
    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
    //Store the new heap index into the parent's field
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(objSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(copySlot);
}

//Primitive/enum/string/array field assignment. Phase 10 audit round-4:
//EvalAreaClaim(2) [receiver, value], receiver first — same discipline
//as the class branch above.
void VmBackend::EmitAssignStructFieldPlain(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, uint16_t fieldOff,
        BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 2);
    uint16_t objSlot = claim.base();
    uint16_t valueSlot = objSlot + VALUE_SIZE;
    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
    EmitExpression(*assign.Right(), emitter, valueSlot);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(objSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(valueSlot);
}

//Compound assignment: x += y, this.f -= 1, obj.f *= 2.
//Phase 9a: left-value is evaluated only once (read-modify-write).
//Subscript compound assign (arr[i] += 1) is intentionally not
//supported — left-value single-eval requires 4 scratch slots and
//complicates the grammar. Users write `arr[i] = arr[i] + 1`.
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
    uint16_t oldValSlot = claimBase + VALUE_SIZE;
    uint16_t rhsSlot = claimBase + 2 * VALUE_SIZE;
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
    uint16_t oldValSlot = claimBase + VALUE_SIZE;
    uint16_t rhsSlot = claimBase + 2 * VALUE_SIZE;
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

} //namespace nlang
