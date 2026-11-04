/*---
    EmitExprMemberArray.cpp — the array face member emitters
    (length/rank/shape + the equals/getHashCode protocol calls), split
    from EmitExprMember.cpp when that file reached the source-size
    budget. The face is rootless (no class record dispatch): every
    method is shape-matched against the receiver's array-ness.
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
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang {

//Array length() builtin method (e.g. arr.length()). The receiver
//check is the array-valued property on the outer expression (stamped
//at resolver binding tails — any shape: identifier, member like
//li.get(0), or call result like mk()/lib.mk(3)); the emission itself
//is shape-agnostic (EmitExpression handles any receiver form). The
//resolver consumed the call shape (TryResolveArrayLengthMethod); the
//zero-argument check here is defensive depth. Checked BEFORE the
//struct/class dispatch in EmitMemberTypedReceiverDispatch as defense
//in depth: array values carry the interned array token in
//EvalDataType (IsArrayValued is that derived check), so the
//struct/class branches do not match array receivers today — the
//order just guarantees a future typed-receiver change cannot
//silently swallow the builtin.
//Returns true when the method call was emitted.
bool VmBackend::EmitMemberArrayLengthMethod(SnMemberExpr& member,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr
        && static_cast<SnInvokeExpr*>(inner)->CalleeName() == "length"
        && static_cast<SnInvokeExpr*>(inner)->Params().begin()
            == static_cast<SnInvokeExpr*>(inner)->Params().end()
        && member.Outer()->IsArrayValued()))
        return false;
    EmitExpression(*member.Outer(), emitter, resultOffset);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ArrayLength);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(resultOffset);
    return true;
}

//Stage-2: arr.rank() — the rank is a compile-time property of the
//receiver's token; the receiver still evaluates (and null-checks) for
//side-effect and null-semantics parity with length().
//v0.12.7: the generic spelling Array<E> carries no static annotation —
//read the rank off the value (the shape array's length composes the
//record rank from two existing primitives; no rank-specific opcode).
bool VmBackend::EmitMemberArrayRankMethod(SnMemberExpr& member,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr
        && static_cast<SnInvokeExpr*>(inner)->CalleeName() == "rank"
        && static_cast<SnInvokeExpr*>(inner)->Params().begin()
            == static_cast<SnInvokeExpr*>(inner)->Params().end()
        && member.Outer()->IsArrayValued()))
        return false;
    auto* pType = member.Outer()->EvalDataType();
    EmitExpression(*member.Outer(), emitter, resultOffset);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(resultOffset);
    if (pType->Kind() == NK_ArrayTypeToken) {
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(static_cast<int32_t>(
            static_cast<SnArrayTypeToken*>(pType)->Rank()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    } else {
        RegisterArrayType(SnBuiltinDataType::InstanceOf(NK_Int32));
        emitter.Emit(OpCode::OP_ArrayShape);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_ArrayLength);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(resultOffset);
    }
    return true;
}

//Stage-2: arr.shape() — mints int[rank] with the dims (OP_ArrayShape).
bool VmBackend::EmitMemberArrayShapeMethod(SnMemberExpr& member,
                                           BytecodeEmitter& emitter,
                                           uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr
        && static_cast<SnInvokeExpr*>(inner)->CalleeName() == "shape"
        && static_cast<SnInvokeExpr*>(inner)->Params().begin()
            == static_cast<SnInvokeExpr*>(inner)->Params().end()
        && member.Outer()->IsArrayValued()))
        return false;
    //The runtime mint looks up int[] in the module's arrayTypes table:
    //register it here (the lazy codegen-time registration every other
    //expression-side array follows — EmitExprNew, init lists), since a
    //declaration elsewhere must not be required to carry it.
    RegisterArrayType(SnBuiltinDataType::InstanceOf(NK_Int32));
    EmitExpression(*member.Outer(), emitter, resultOffset);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ArrayShape);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(resultOffset);
    return true;
}

//v0.12.7: Array protocol methods — equals(Object)/getHashCode(),
//identity semantics (the Object protocol intrinsics: heap-idx compare
//and hash, null-aware). Shape-matched like length/rank/shape (the face
//is rootless, no class record dispatch) and staged exactly like the
//string protocol calls: receiver to resultOffset first, then the
//{this, args...} claim, bulk copy to callParamBase, intrinsic, assign
//back (the round-10 receiver-first recipe).
bool VmBackend::EmitMemberArrayProtocolMethod(SnMemberExpr& member,
                                              BytecodeEmitter& emitter,
                                              uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr
        && member.Outer()->IsArrayValued()))
        return false;
    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
    const auto& name = invoke.CalleeName();
    if (name == "getHashCode" && invoke.Params().begin() == invoke.Params().end()) {
        EmitExpression(*member.Outer(), emitter, resultOffset);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_CallIntrinsic);
        emitter.EmitUint16(INTR_Object_GetHashCode);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_ParaEnd);
        return true;
    }
    uint16_t argCount = 0;
    for (auto& p : invoke.Params()) ++argCount;
    if (name == "equals" && argCount == 1) {
        EvalAreaClaim claim(*this, 2);
        uint16_t claimBase = claim.base();
        EmitExpression(*member.Outer(), emitter, resultOffset);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);
        for (auto& param : invoke.Params()) {
            EmitExpression(param, emitter,
                claimBase + kFrameSlotBytes);
        }
        CopyClaimToCallParams(claimBase, 2, emitter);
        emitter.Emit(OpCode::OP_CallIntrinsic);
        emitter.EmitUint16(INTR_Object_Equals);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_ParaEnd);
        return true;
    }
    return false;
}

} //namespace nlang
