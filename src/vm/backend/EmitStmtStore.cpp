/*---
    EmitStmtStore.cpp — 下标存储语句发射（数组元素与容器 set 语法糖两路）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
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
    bool isList = (baseName == "List" && !typeArgs.empty());
    bool isDict = (baseName == "Dict" && typeArgs.size() > 1);
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    //Array-typed slots are interned tokens — raw handles, no box
    //(BoxingTagFor default). Dict keys box when primitive; List's
    //index is int.
    auto keyBox = isDict
        ? BoxingTagFor(typeArgs[0])
        : BoxingTagResult{0, false};
    //Value boxes when T/V is primitive (argPlans[2] in the
    //member-call path; slot 2 here).
    SnField* pElem = isList ? typeArgs[0]
        : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
    auto valBox = BoxingTagFor(pElem);
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
                                 const BoxingTagResult& box,
                                 BytecodeEmitter& emitter) {
    EmitExpression(operand, emitter, slot);
    if (!box.isPrimitive) return;
    EmitPResultRefresh(emitter, slot);
    emitter.Emit(OpCode::OP_Box);
    emitter.EmitByte(box.tag);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slot);
}

//Element type of the array being stored into: the array-valued
//property covers every base shape (identifier, member, subscript,
//call — the resolver's element-type gate stamps the property on every
//shape). Mirrors the resolver's test in Access(SnSubscriptAssignStmt)
//so struct deep-copy fires for every base shape, not just identifier
//and member bases. 0.7.3 B token path: an array-valued base carries
//the interned token; struct deep-copy detection keys on the ELEMENT.
static SnField* ArrayElementStoreType(SnSubscriptAssignStmt& sub) {
    SnField* elemType = nullptr;
    if (sub.Array()->IsArrayValued()) {
        elemType = sub.Array()->EvalDataType();
        if (elemType && elemType->Kind() == NK_ArrayTypeToken)
            elemType = static_cast<SnArrayTypeToken*>(
                elemType)->ElemTypeOf();
    }
    return elemType;
}

//Array element store (non-container bases): claims [array, index,
//value] then OP_StoreElement, with struct deep-copy of the value when
//the element type is a struct. Phase 10 audit round-2: runs entirely
//inside an EvalAreaClaim(3) [array, index, value] — the old
//tempSlot/tempSlot2/callParamBase staging let any nested expression
//in the index (subscript-get, binary arithmetic — both reuse the
//shared temps) clobber the parked base/value. Claim slots are
//exclusive, same discipline as the container path above.
void VmBackend::EmitArrayElementStore(SnSubscriptAssignStmt& sub,
                                      BytecodeEmitter& emitter) {
    SnField* elemType = ArrayElementStoreType(sub);
    EvalAreaClaim claim(*this, 3);
    uint16_t claimBase = claim.base();
    uint16_t indexSlot = claimBase + kFrameSlotBytes;
    uint16_t valueSlot = claimBase + 2 * kFrameSlotBytes;
    //Object[] element stores arrive pre-boxed: the resolver's
    //element-type gate (Access(SnSubscriptAssignStmt)) wraps
    //primitive values in a TCK_Box cast, so this emit includes the
    //OP_Box — the same representation field/local stores and the
    //container path use. Null literals skip the wrap at the
    //resolver and keep their raw identity.
    EmitExpression(*sub.Value(), emitter, valueSlot);
    EmitExpression(*sub.Index(), emitter, indexSlot);
    //Array reference last, null-checked (mirrors container path).
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //For struct element types, deep-copy value before storing.
    //Per-unit: a cross-unit element struct slots as an import
    //placeholder (the old silent-0 fallback could not).
    if (elemType && RuntimeTypeKind(elemType) == RTK_Struct
        && elemType->Kind() == NK_StructDecl) {
        EmitStructDeepCopy(valueSlot, valueSlot,
            static_cast<SnStructDecl&>(*elemType), emitter);
    }
    emitter.Emit(OpCode::OP_StoreElement);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(indexSlot);
    emitter.EmitUint16(valueSlot);
}

} //namespace nlang
