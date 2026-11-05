/*---
    EmitStmtAssign.cpp — 赋值语句发射：赋值目标形态族与复合赋值。
    从 EmitStmtDecl.cpp 拆出（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
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
//included), bare static-field slot store inside the class, or implicit
//this.<field> write inside a method.
void VmBackend::EmitAssignBareIdentifier(SnAssignStmt& assign,
        SnField* field, BytecodeEmitter& emitter) {
    auto target = ResolveBareIdentifier(field);
    if (target.kind == BareIdTarget::Local) {
        EmitAssignToLocal(assign, target.localOffset,
            field->EvalDataType(), emitter);
    } else if (target.kind == BareIdTarget::StaticSlot) {
        //v0.12.6 Task 7: a static LOCAL's declaration-site init (the
        //decomposed NF_Generated assign) runs under the slot's own
        //once-guard (D2/D10) — the guard owns exactly this store. A
        //later user store is a plain slot write: the declaration is
        //lexically upstream, so the machine is Done (and D12's init
        //bit already covers the re-entrant window).
        if (field->Kind() == NK_FormalParam
            && assign.ContainFlags(NF_Generated)) {
            EmitStaticLocalInitSequence(assign, target.slotIdx, emitter);
            return;
        }
        //v0.12.6: bare static-field store — same stage-then-store shape
        //as the local write (the destination is written only after the
        //whole RHS evaluated). The gate runs before the RHS: reaching
        //the store is itself the first active use (D2); for a static
        //local the field-bound gate is a no-op (no class owner).
        EmitStaticFieldGate(field, emitter);
        EvalAreaClaim claim(*this, 1);
        uint16_t valueSlot = claim.base();
        EmitExpression(*assign.Right(), emitter, valueSlot);
        emitter.Emit(OpCode::OP_StoreStatic);
        emitter.EmitUint16(target.slotIdx);
        emitter.EmitUint16(valueSlot);
    } else if (target.kind == BareIdTarget::ThisField) {
        EmitAssignToThisField(assign, target.fieldOff,
            field->EvalDataType(), emitter);
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
        //v0.12.7: annotated target × unannotated value — the boundary
        //rank check (null passes through unchanged).
        EmitArrayRankBoundary(varType, assign.Right(), valueSlot, emitter);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(valueSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(offset);
    }
}

//Implicit this.<field> = value (bare member write inside a method).
//Same opcode shape as the MemberExpr write; the declared type rides
//along for the D14 struct deep-copy decision.
void VmBackend::EmitAssignToThisField(SnAssignStmt& assign, int fieldOff,
        SnField* fieldType, BytecodeEmitter& emitter) {
    EmitExpression(*assign.Right(), emitter,
        m_currFunc->tempSlot2);
    //v0.12.7: boundary rank check on the staged value.
    EmitArrayRankBoundary(fieldType, assign.Right(),
        m_currFunc->tempSlot2, emitter);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(ImplicitThisSlot());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(m_currFunc->tempSlot);
    EmitStructAwareStoreField(fieldType, m_currFunc->tempSlot,
        static_cast<uint16_t>(fieldOff), m_currFunc->tempSlot2, emitter);
}

//Member lvalue: arr[i].field / li[i].field subscripted-element stores
//first (they fully consume the statement), then the type-qualified
//static-field store (v0.12.6), then plain class/struct field stores.
void VmBackend::EmitAssignMemberField(SnAssignStmt& assign,
        SnMemberExpr& memberExpr, BytecodeEmitter& emitter) {
    if (EmitAssignSubscriptElementStore(assign, memberExpr, emitter))
        return;
    //v0.12.6: an enum member is a compile-time constant — the resolver's
    //const-store gate rejects user stores. Reaching emission means the
    //gate missed, and the historical fall-through emitted NOTHING for
    //the store silently; fail the build instead.
    if (auto* guard = memberExpr.Field(); guard
        && guard->Kind() == NK_EnumMember) {
        throw std::runtime_error(
            "NLang backend: enum member reached store emission: "
            + guard->Name());
    }
    //v0.12.6 D3: static store through the class name. Keyed on the
    //bound field, not the receiver's type — the outer names a type and
    //has no value shape (a value receiver never reaches codegen; the
    //resolver's namespace filter rejects it).
    //Task 7 (D11): a RUNTIME interface constant stores through the
    //interface's synthesized $staticInit only (user stores were
    //rejected at the resolver's const gate); folded constants never
    //have a slot, so the map membership is the discriminator.
    if (auto* field = memberExpr.Field(); field
        && field->Kind() == NK_ClassField
        && field->ContainFlags(NF_Static)
        && (!IsInterfaceConstantField(*field)
            || m_staticSlotByField.count(
                static_cast<SnClassField*>(field)) != 0)) {
        //Gate before the RHS (first active use, D2; declaring class, D4).
        EmitStaticFieldGate(field, emitter);
        EvalAreaClaim claim(*this, 1);
        uint16_t valueSlot = claim.base();
        EmitExpression(*assign.Right(), emitter, valueSlot);
        emitter.Emit(OpCode::OP_StoreStatic);
        emitter.EmitUint16(StaticSlotIndexOf(
            static_cast<SnClassField&>(*field)));
        emitter.EmitUint16(valueSlot);
        return;
    }
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

//v0.12.6 D14: the declared-type twin of ResolveMemberFieldOffset —
//class fields walk the inheritance chain (FindClassFieldDecl), struct
//fields scan the flat list; a miss returns null and the caller's
//store falls back to the plain 8-byte cell store.
SnField* VmBackend::DeclaredMemberFieldType(SyntaxNode* elemType,
        const std::string& fieldName) {
    if (elemType->Kind() == NK_ClassDecl)
        return FindClassFieldDecl(
            *static_cast<SnClassDecl*>(elemType), fieldName);
    if (elemType->Kind() == NK_StructDecl) {
        for (auto& sf :
            static_cast<SnStructDecl*>(elemType)->Fields()) {
            if (sf.Name() == fieldName)
                return sf.EvalDataType();
        }
    }
    return nullptr;
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
    SnField* fieldType = DeclaredMemberFieldType(elemType, fieldName);
    if (IsContainerSubscript(*sub.Array())) {
        EmitAssignContainerElementFieldStore(assign, sub, elemType,
            fieldOff, fieldType, emitter);
        return true;
    }
    EmitAssignArrayElementFieldStore(assign, sub, elemType, fieldOff,
        fieldType, emitter);
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
        SnField* fieldType, BytecodeEmitter& emitter) {
    EvalAreaClaim claim(*this, 2);
    uint16_t objSlot = claim.base();
    uint16_t valueSlot = objSlot + kFrameSlotBytes;
    EmitExpression(sub, emitter, objSlot);
    if (elemType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(objSlot);
    }
    EmitExpression(*assign.Right(), emitter, valueSlot);
    //v0.12.7: boundary rank check on the staged value (container
    //elements may carry an annotated field).
    EmitArrayRankBoundary(fieldType, assign.Right(), valueSlot, emitter);
    EmitStructAwareStoreField(fieldType, objSlot, fieldOff, valueSlot,
        emitter);
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
        SnField* fieldType, BytecodeEmitter& emitter) {
    //Stage-2: N indices + element temp + value in one exclusive claim.
    const size_t rank = sub.Indices().size();
    EvalAreaClaim claim(*this, rank + 2);
    uint16_t claimBase = claim.base();
    uint16_t idxBase = claimBase + kFrameSlotBytes;
    uint16_t valueSlot = claimBase + (rank + 1) * kFrameSlotBytes;
    //1. Evaluate right side → value slot
    EmitExpression(*assign.Right(), emitter, valueSlot);
    //2. Evaluate indices → idxBase..
    for (size_t d = 0; d < rank; ++d)
        EmitExpression(*sub.Indices()[d], emitter,
            idxBase + d * kFrameSlotBytes);
    //3. Evaluate array ref → claim[0], null-checked
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //4. load element into tempSlot (free scratch — all operands are
    //parked in exclusive claim slots now)
    emitter.Emit(SubscriptNeedsDynOps(*sub.Array())
        ? OpCode::OP_LoadElementDyn : OpCode::OP_LoadElementN);
    emitter.EmitUint16(m_currFunc->tempSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(idxBase);
    emitter.EmitUint16(static_cast<uint16_t>(rank));
    //5. null_check the element (class) or skip (struct is value)
    if (elemType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(m_currFunc->tempSlot);
    }
    //6. store_field obj=tempSlot off=fieldOff src=claim[2] (struct
    //values deep-copy in place first, D14)
    EmitStructAwareStoreField(fieldType, m_currFunc->tempSlot, fieldOff,
        valueSlot, emitter);
}

//Class field assignment: reference semantics for handles, deep copy for
//struct values (v0.12.6 D14 — the field owns an independent record,
//manual 4.7.4). Phase 10 audit round-4: park [receiver, value] in an
//EvalAreaClaim(2) — the old tempSlot/tempSlot2 staging was clobbered by
//a call in the RHS (argument binaries fall through PickTempSlot to
//tempSlot; struct-argument deep copy scratches tempSlot), crashing or
//storing through the wrong object. Receiver first (JLS 15.26.1).
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
    uint16_t valueSlot = objSlot + kFrameSlotBytes;
    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(objSlot);
    EmitExpression(*assign.Right(), emitter, valueSlot);
    //v0.12.7: boundary rank check on the staged value.
    {
        auto* pFieldDecl = FindClassFieldDecl(*classDecl, fieldName);
        EmitArrayRankBoundary(
            pFieldDecl ? pFieldDecl->EvalDataType() : nullptr,
            assign.Right(), valueSlot, emitter);
        EmitStructAwareStoreField(pFieldDecl,
            objSlot, fieldOff, valueSlot, emitter);
    }
}

//v0.12.6 D14: struct-aware field store — the emit-side twin of the
//static slot's runtime deep-copy (OpStoreStatic). When the field's
//declared type is a struct, the staged value is deep-copied in place
//first, so the field owns an independent record (manual 4.7.4); every
//other kind stores the 8-byte cell as-is. Same Kind()-after-
//RuntimeTypeKind shape as EmitAssignToLocal's struct branch.
void VmBackend::EmitStructAwareStoreField(SnField* fieldType,
        uint16_t objSlot, uint16_t fieldOff, uint16_t valueSlot,
        BytecodeEmitter& emitter) {
    if (fieldType && RuntimeTypeKind(fieldType) == RTK_Struct
        && fieldType->Kind() == NK_StructDecl) {
        EmitStructDeepCopy(valueSlot, valueSlot,
            static_cast<SnStructDecl&>(*fieldType), emitter);
    }
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
    for (auto& sf : structDecl->Fields()) {
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
    uint16_t objSlot = copySlot + kFrameSlotBytes;
    uint16_t rhsSlot = objSlot + kFrameSlotBytes;
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
    uint16_t valueSlot = objSlot + kFrameSlotBytes;
    EmitExpression(*memberExpr.Outer(), emitter, objSlot);
    EmitExpression(*assign.Right(), emitter, valueSlot);
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(objSlot);
    emitter.EmitUint16(fieldOff);
    emitter.EmitUint16(valueSlot);
}


} //namespace nlang
