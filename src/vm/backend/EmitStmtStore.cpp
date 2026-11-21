/*---
    EmitStmtStore.cpp — 下标存储语句发射（数组元素与容器 set 语法糖两
    路），以及成员赋值的下标元素存储族（li[i].f = v / arr[i].f = v，
    含其字段offset辅助）。2026-09-25 从 VmBackend.cpp 抽取；2026-11-21
    自 EmitStmtAssign.cpp 并入下标元素存储族（source-size guard重构，
    零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/runtime/BuiltinGenericNames.h>
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
#include <stdexcept>
#include <unordered_set>

namespace nlang {


void VmBackend::Access(SnSubscriptAssignStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& sub = static_cast<SnSubscriptAssignStmt&>(stmt);
        //List<T>/Dict<K,V> subscript store sugar dispatches to the
        //set() intrinsic; every other base shape is an array element
        //store.
        if (IsContainerSubscript(*sub.Array())) {
            EmitContainerSubscriptSet(sub, emitter);
            return;
        }
        EmitArrayElementStore(sub, emitter);
}

//List<T>/Dict<K,V> subscript store: li[i] = v == li.set(i, v),
//d[k] = v == d.set(k, v) — sugar over the set() intrinsic call.
//Runs entirely inside an EvalAreaClaim(3) [this, index, value] so
//nested calls in any operand cannot clobber the params, and no
//shared temp slots are touched (claim slots are exclusive).
void VmBackend::EmitContainerSubscriptSet(SnSubscriptAssignStmt& sub,
                                          BytecodeEmitter& emitter) {
    auto* pGenClass = static_cast<SnClassDecl*>(
        sub.Array()->EvalDataType());
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    bool isList = (baseName == kBuiltinListTypeName && !typeArgs.empty());
    bool isDict = (baseName == kBuiltinDictTypeName && typeArgs.size() > 1);
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    //Array-typed slots are interned tokens — raw handles, no box
    //(BoxingTagFor default). Dict keys box when primitive; List's
    //index is int.
    auto keyBox = isDict
        ? PlanBoundaryBox(typeArgs[0], pGenClass)
        : ArgBoxPlan{};
    //Value normalizes when T/V is primitive (argPlans[2] in the
    //member-call path; slot 2 here).
    SnField* pElem = isList ? typeArgs[0]
        : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
    auto valBox = PlanBoundaryBox(pElem, pGenClass);
    //arg0 = index → claim[1]
    EmitBoxedOperand(*sub.Index(), claimBase + kFrameSlotBytes, keyBox,
                     emitter);
    //arg1 = value → claim[2]
    EmitBoxedOperand(*sub.Value(), claimBase + 2 * kFrameSlotBytes, valBox,
                     emitter);
    //this = receiver → claim[0], null-checked.
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //Bulk-copy claim → callParamBase, then set().
    for (uint16_t i = 0; i < 3; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(
            m_currFunc->callParamBase + i * kFrameSlotBytes);
    }
    uint16_t nameIdx = AddStringConstant("set");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Emit one container-method operand into its claim slot, boxing it in
//place when the boxing plan says primitive. Used by the container
//subscript SET sugar path; the get path keeps its own inline boxing
//(EmitExprCast.cpp).
void VmBackend::EmitBoxedOperand(SnExpression& operand, uint16_t slot,
                                 const ArgBoxPlan& box,
                                 BytecodeEmitter& emitter) {
    EmitExpression(operand, emitter, slot);
    if (!box.needsBox) return;
    EmitBoundaryBox(box, emitter, slot);
}

//Element type of the array being stored into: the array-valued
//property covers every base shape (identifier, member, subscript,
//call — the resolver's element-type gate stamps the property on every
//shape). Mirrors the resolver's test in Access(SnSubscriptAssignStmt)
//so struct stabilization fires for every base shape, not just
//identifier and member bases. The detection keys on the ELEMENT row,
//either spelling of the base's array type (token or Array<E>) — a
//token-only peel skipped the struct path for the generic spelling.
//Also consumed by SubscriptAssignStmtPeakDepth (claim-size mirror).
SnField* VmBackend::ArrayElementStoreType(SnSubscriptAssignStmt& sub) {
    if (!sub.Array()->IsArrayValued())
        return nullptr;
    return ArrayElementRowOf(sub.Array()->EvalDataType());
}

//Array element store (non-container bases): claims [array, index,
//value] then OP_StoreElement. For struct element types the record-
//stabilization path replaces the store: OP_LoadElement loads the
//element's established record handle (and performs the bounds check
//the dropped store used to perform), then OP_CopyStructInPlace
//writes the value's cells into that record — zero allocation, and
//identity-preserving. Phase 10 audit round-2: runs entirely inside
//an EvalAreaClaim [array, indices..., value, (elem)] — the old
//tempSlot/tempSlot2/callParamBase staging let any nested expression
//in the index (subscript-get, binary arithmetic — both reuse the
//shared temps) clobber the parked base/value. Claim slots are
//exclusive, same discipline as the container path above.
//Struct-element tail: load the element's ESTABLISHED record handle
//into the claim's element slot — the load performs the bounds check the
//dropped store used to perform — then copy the value into that record
//IN PLACE. The record identity never changes, so held references to the
//element stay live. Per-unit: a cross-unit element struct slots as an
//import placeholder (the old silent-0 fallback could not).
void VmBackend::EmitStabilizedElementStore(bool dynOps, uint16_t elemSlot,
        uint16_t claimBase, uint16_t idxBase, uint16_t valueSlot,
        uint16_t rank, SnField* elemType, BytecodeEmitter& emitter) {
    emitter.Emit(dynOps ? OpCode::OP_LoadElementDyn
                        : OpCode::OP_LoadElementN);
    emitter.EmitUint16(elemSlot);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(idxBase);
    emitter.EmitUint16(rank);
    EmitStructCopyInPlace(elemSlot, valueSlot,
        static_cast<SnStructDecl&>(*elemType), emitter);
}

void VmBackend::EmitArrayElementStore(SnSubscriptAssignStmt& sub,
                                      BytecodeEmitter& emitter) {
    SnField* elemType = ArrayElementStoreType(sub);
    const bool elemIsStruct = elemType
        && RuntimeTypeKind(elemType) == RTK_Struct
        && elemType->Kind() == NK_StructDecl;
    //Stage-2: N indices + value + receiver (+ one element-handle slot
    //on the struct path) in one exclusive claim; the receiver parks
    //LAST (null-checked) like the rank-1 form.
    const size_t rank = sub.Indices().size();
    EvalAreaClaim claim(*this, rank + (elemIsStruct ? 3 : 2));
    uint16_t claimBase = claim.base();
    uint16_t idxBase = claimBase + kFrameSlotBytes;
    uint16_t valueSlot = claimBase + (rank + 1) * kFrameSlotBytes;
    uint16_t elemSlot = claimBase + (rank + 2) * kFrameSlotBytes;
    //Object[] element stores arrive pre-boxed: the resolver's
    //element-type gate (Access(SnSubscriptAssignStmt)) wraps
    //primitive values in a TCK_Box cast, so this emit includes the
    //OP_Box — the same representation field/local stores and the
    //container path use. Null literals skip the wrap at the
    //resolver and keep their raw identity.
    EmitExpression(*sub.Value(), emitter, valueSlot);
    for (size_t d = 0; d < rank; ++d)
        EmitExpression(*sub.Indices()[d], emitter,
            idxBase + d * kFrameSlotBytes);
    //Array reference last, null-checked (mirrors container path).
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    if (elemIsStruct) {
        EmitStabilizedElementStore(SubscriptNeedsDynOps(*sub.Array()),
            elemSlot, claimBase, idxBase, valueSlot,
            static_cast<uint16_t>(rank), elemType, emitter);
        return;
    }
    emitter.Emit(SubscriptNeedsDynOps(*sub.Array())
        ? OpCode::OP_StoreElementDyn : OpCode::OP_StoreElementN);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(idxBase);
    emitter.EmitUint16(valueSlot);
    emitter.EmitUint16(static_cast<uint16_t>(rank));
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

} //namespace nlang
