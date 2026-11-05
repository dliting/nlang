/*---
    EmitArrayBoundary.cpp — the v0.12.7 boundary rank check emitter,
    split from EmitExprCast.cpp when that file reached the source-size
    budget. One helper, wired at the value/annotation crossing points
    (assignment targets, formal bindings, returns, `as` with an
    annotated target): the runtime rank of an unannotated Array<E>
    value verifies against the target's annotation, raising a
    catchable InvalidCastException on mismatch.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnExpressions.h>

namespace nlang
{

//Boundary rank check emission (see VmBackend.h). The value sits in
//`slot` per the EmitExpression convention; the check rewrites it in
//place, so staging order stays untouched.
void VmBackend::EmitArrayRankBoundary(const SnField* targetType,
                                      const SnExpression* pSource,
                                      uint16_t slot,
                                      BytecodeEmitter& emitter) {
    //The declared type arrives either as the resolved token itself or as
    //the owning field declaration — normalize to the node holding the
    //rank (the token's own EvalDataType is the type-of-type, hence the
    //kind check before the dereference).
    SnField* pDeclared = const_cast<SnField*>(targetType);
    if (pDeclared && pDeclared->Kind() != NK_ArrayTypeToken)
        pDeclared = pDeclared->EvalDataType();
    if (!pDeclared || pDeclared->Kind() != NK_ArrayTypeToken)
        return;
    auto* pSrcType = pSource ? pSource->EvalDataType() : nullptr;
    if (!pSrcType || !ArrayInstantiationElem(pSrcType))
        return;
    emitter.Emit(OpCode::OP_CheckArrayRank);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(static_cast<SnArrayTypeToken*>(
        pDeclared)->Rank());
}

//v0.12.7 Task 5: the Object→array downcast emitter — `o as int[]`. The
//target's array type registers at codegen (the lazy registration every
//expression-side array follows) and OP_CheckArrayType verifies kind and
//type identity at run time. False = the target is not an array token
//(the caller falls through to the class OP_CheckCast arm).
bool VmBackend::EmitAsArrayDowncastOp(const SnField* targetType,
                                      uint16_t slot,
                                      BytecodeEmitter& emitter) {
    if (!targetType || targetType->Kind() != NK_ArrayTypeToken)
        return false;
    auto* pToken = static_cast<const SnArrayTypeToken*>(targetType);
    uint16_t arrayTypeIdx = RegisterArrayType(pToken->ElemTypeOf(),
                                              pToken->Rank());
    emitter.Emit(OpCode::OP_CheckArrayType);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(slot);
    emitter.EmitUint16(arrayTypeIdx);
    return true;
}

} //namespace nlang
