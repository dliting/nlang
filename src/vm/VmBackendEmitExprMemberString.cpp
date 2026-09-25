/*---
    VmBackendEmitExprMemberString.cpp — 成员表达式字符串族发射：非 class 接收者 toString 分派、string 内建方法（内建表驱动）与字符串方法尾部分派。
    从 VmBackendEmitExprMemberCall.cpp 拆出（2026-09-25 可维护性重构，零行为变化；源出 VmBackendEmitExprMember.cpp，再上溯 VmBackend.cpp）。
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

//Phase 8e-9b: non-class receiver toString() dispatch.
//For enum/int/float receivers, the resolver accepted the call
//(EvalDataType=String, NF_Resolved) without setting m_pField.
//Codegen dispatches based on outer->EvalDataType():
// - enum (NK_EnumDecl or NK_EnumMember Field()) → OP_Enum_to_str
// - int (NK_Int32) → OP_Int32_to_str
// - float (NK_Float) → OP_Float_to_str
// - string (NK_String) → identity (no opcode)
//This mirrors the SnCastExpr handler's dispatch but for the
//MemberExpr+InvokeExpr AST shape that `x.toString()` produces.
//Returns true when the call was emitted.
bool VmBackend::EmitMemberToStringNonClass(SnMemberExpr& member,
                                           BytecodeEmitter& emitter,
                                           uint16_t resultOffset) {
    auto* outerType = member.Outer()->EvalDataType();
    NodeKind outerKind = outerType ? outerType->Kind() : static_cast<NodeKind>(0);
    if (EmitMemberArrayToString(member, emitter, resultOffset))
        return true;
    //String.toString() — identity, no opcode.
    if (outerKind == NK_String)
    {
        EmitExpression(*member.Outer(), emitter, resultOffset);
        return true;
    }
    //Enum receiver — need to find the SnEnumDecl for enumDefIdx.
    //Two sub-cases:
    // (a) outerKind == NK_EnumDecl (typed enum variable)
    // (b) outerKind == NK_Int32 but outer's Field() is NK_EnumMember
    //     (enum literal like Color.Green — EvalDataType is NK_Int32)
    if (outerKind == NK_EnumDecl)
        return EmitMemberEnumToString(member, outerType, emitter,
                                      resultOffset);
    return EmitMemberNumericToString(member, outerKind, emitter, resultOffset);
}

//Array receiver arm of the toString dispatch.
//Phase 9b-pre: array receiver — array is a VM primitive,
//not a class. MUST be checked BEFORE the int path because
//EvalDataType for `int[] arr` returns the element type
//(NK_Int32); array-ness is on the SnField via IsArrayType().
//Returns true when the receiver was array-valued and the
//conversion was emitted.
bool VmBackend::EmitMemberArrayToString(SnMemberExpr& member,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    auto outerExprKind = member.Outer()->Kind();
    if (outerExprKind == NK_MemberExpr
        || outerExprKind == NK_IdentifierExpr)
    {
        auto& outerFieldExpr = static_cast<SnFieldExpr&>(
            *member.Outer());
        auto* outerField = outerFieldExpr.Field();
        if (outerField && outerField->IsArrayType())
        {
            EmitExpression(*member.Outer(), emitter, resultOffset);
            //Field loads leave the pResult accumulator
            //stale (see EmitPResultRefresh); array_to_str
            //reads the accumulator, so reload first.
            EmitPResultRefresh(emitter, resultOffset);
            emitter.Emit(OpCode::OP_Array_to_str);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return true;
        }
    }
    return false;
}

//Typed enum variable arm (outerKind == NK_EnumDecl): OP_Enum_to_str with
//the decl's enumDefIdx; the arm still consumes the call when the decl has
//no table entry.
bool VmBackend::EmitMemberEnumToString(SnMemberExpr& member,
                                       SnField* outerType,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    EmitExpression(*member.Outer(), emitter, resultOffset);
    auto* enumDecl = static_cast<SnEnumDecl*>(outerType);
    auto it = m_enumIndexMap.find(enumDecl);
    if (it != m_enumIndexMap.end())
    {
        emitter.Emit(OpCode::OP_Enum_to_str);
        emitter.EmitUint16(static_cast<uint16_t>(it->second));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
    }
    return true;
}

//Enum literal receiver arm (Color.Green): EvalDataType is NK_Int32 while
//the literal's Field() is the NK_EnumMember — same OP_Enum_to_str shape.
//Returns true when the receiver was an enum literal.
bool VmBackend::EmitMemberEnumLiteralToString(SnMemberExpr& member,
                                              BytecodeEmitter& emitter,
                                              uint16_t resultOffset) {
    auto outerExprKind = member.Outer()->Kind();
    if (outerExprKind == NK_MemberExpr
        || outerExprKind == NK_IdentifierExpr)
    {
        auto& outerFieldExpr = static_cast<SnFieldExpr&>(
            *member.Outer());
        auto* outerField = outerFieldExpr.Field();
        if (outerField
            && outerField->Kind() == NK_EnumMember)
        {
            EmitExpression(*member.Outer(), emitter, resultOffset);
            auto* enumDecl = static_cast<SnEnumDecl*>(
                outerField->Parent());
            auto it = m_enumIndexMap.find(enumDecl);
            if (it != m_enumIndexMap.end())
            {
                emitter.Emit(OpCode::OP_Enum_to_str);
                emitter.EmitUint16(static_cast<uint16_t>(it->second));
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(resultOffset);
            }
            return true;
        }
    }
    return false;
}

//int/float receiver arms: refresh pResult, then OP_Int32_to_str /
//OP_Float_to_str. Returns false for any other receiver kind (the call
//falls through to the class-receiver paths).
bool VmBackend::EmitMemberNumericToString(SnMemberExpr& member,
                                          NodeKind outerKind,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    if (outerKind == NK_Int32)
    {
        //Check if outer is an enum literal (Field() == NK_EnumMember)
        if (EmitMemberEnumLiteralToString(member, emitter, resultOffset))
            return true;
        //Plain int receiver
        EmitExpression(*member.Outer(), emitter, resultOffset);
        EmitPResultRefresh(emitter, resultOffset);
        emitter.Emit(OpCode::OP_Int32_to_str);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return true;
    }
    if (outerKind == NK_Float)
    {
        EmitExpression(*member.Outer(), emitter, resultOffset);
        EmitPResultRefresh(emitter, resultOffset);
        emitter.Emit(OpCode::OP_Float_to_str);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return true;
    }
    return false;
}

//Invoke-shaped tail after every other phase: string builtin methods on a
//string receiver, otherwise re-emit the inner invoke expression.
//String builtin methods: s.length(), s.GetHashCode(), s.Equals(other)
//Phase 8e-1: GetHashCode/Equals dispatch to intrinsics for value semantics.
//Strings are primitives (pool idx), not classes — these intrinsics are the
//only way to invoke the protocol on them. Parallel to s.length() shortcut.
void VmBackend::EmitMemberStringMethodTail(SnMemberExpr& member,
                                           SnFieldExpr* inner,
                                           SnField* outerType,
                                           BytecodeEmitter& emitter,
                                           uint16_t resultOffset) {
    if (inner && inner->Kind() == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(*inner);
        if (outerType && outerType->Kind() == NK_String)
        {
            const auto& methName = invoke.CalleeName();
            if (methName == "length")
            {
                EmitExpression(*member.Outer(), emitter, resultOffset);
                emitter.Emit(OpCode::OP_StrLen);
                emitter.EmitUint16(resultOffset);
                emitter.EmitUint16(resultOffset);
                return;
            }
            if (methName == "getHashCode")
            {
                EmitMemberStringHashCode(member, emitter, resultOffset);
                return;
            }
            if (methName == "equals")
            {
                EmitMemberStringEquals(invoke, member, emitter, resultOffset);
                return;
            }
            if (EmitMemberTableStringMethod(invoke, member, emitter,
                                            resultOffset)) {
                return;
            }
        }
        EmitExpression(*static_cast<SnExpression*>(inner), emitter, resultOffset);
    }
}

//String getHashCode intrinsic.
//Round-10 receiver-first: emit the receiver to
//resultOffset, then copy straight to callParamBase — no
//evalArea claim needed (nothing is emitted between the
//copy and the intrinsic call, so nothing can clobber
//callParamBase). The old shape claimed the slot BEFORE
//emitting the receiver, stacking the receiver's nested
//claims on top while the MemberExpr walker is max-shaped
//(max(receiver, 1)) — a receiver containing calls
//drifted past the reserved frame.
void VmBackend::EmitMemberStringHashCode(SnMemberExpr& member,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    EmitExpression(*member.Outer(), emitter, resultOffset);
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(INTR_String_GetHashCode);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//String equals intrinsic.
//Phase 9c follow-up + round-10 receiver-first: args stage
//in the {this, args...} claim (nested calls in later args
//would clobber callParamBase), but the RECEIVER is
//emitted to resultOffset before the claim — resultOffset
//is a user local / parent claim slot, out of reach of
//nested emissions. Claiming before the receiver emission
//stacked the receiver's nested claims above the slice
//while the MemberExpr walker is max-shaped
//(max(receiver, claimSize + argDepth)).
void VmBackend::EmitMemberStringEquals(SnInvokeExpr& invoke,
                                       SnMemberExpr& member,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    uint16_t argCount = 0;
    for (auto& p : invoke.Params()) ++argCount;
    uint16_t n = static_cast<uint16_t>(1 + argCount);
    EmitExpression(*member.Outer(), emitter, resultOffset);
    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);
    uint16_t paramIdx = 1;
    for (auto& param : invoke.Params()) {
        EmitExpression(param, emitter,
            claimBase + paramIdx * VALUE_SIZE);
        ++paramIdx;
    }
    CopyClaimToCallParams(claimBase, n, emitter);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(INTR_String_Equals);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//OP_CallIntrinsic carries no argument count: a call
//shorter than maxArgs must stage the missing trailing
//argument synthetically or the intrinsic reads stale
//memory (substring's end = receiver.length(), filled
//via OP_StrLen on the receiver copy in claim slot 0).
static uint16_t StagedArgCountForStringMethod(const StringMethodEntry& method,
                                              uint16_t argCount) {
    uint16_t stagedArgs = argCount;
    if (method.trailingDefault == STD_ReceiverLength
        && argCount < method.maxArgs)
        stagedArgs = method.maxArgs;
    return stagedArgs;
}

//Phase 11 Step 3: table-driven string methods — the exact
//equals shape above (receiver to resultOffset first, then
//the {this, args...} claim, bulk copy, intrinsic, assign
//back). All 12 return a value, so OP_Assign is always
//emitted. The walker mirrors the claim size (incl. the
//synthesized trailing arg) via the same table — see the
//ExprPeakDepth MemberExpr branch.
//Returns true when the method was found and emitted.
bool VmBackend::EmitMemberTableStringMethod(SnInvokeExpr& invoke,
                                            SnMemberExpr& member,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    const StringMethodEntry* pMethod = FindStringMethod(invoke.CalleeName());
    if (!pMethod)
        return false;
    uint16_t argCount = 0;
    for (auto& p : invoke.Params()) ++argCount;
    uint16_t stagedArgs = StagedArgCountForStringMethod(*pMethod, argCount);
    uint16_t n = static_cast<uint16_t>(1 + stagedArgs);
    EmitExpression(*member.Outer(), emitter, resultOffset);
    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);
    uint16_t paramIdx = 1;
    for (auto& param : invoke.Params()) {
        EmitExpression(param, emitter,
            claimBase + paramIdx * VALUE_SIZE);
        ++paramIdx;
    }
    if (stagedArgs > argCount) {
        //dst = StrLen(receiver copy at claim slot 0)
        emitter.Emit(OpCode::OP_StrLen);
        emitter.EmitUint16(static_cast<uint16_t>(
            claimBase + stagedArgs * VALUE_SIZE));
        emitter.EmitUint16(claimBase);
    }
    CopyClaimToCallParams(claimBase, n, emitter);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(pMethod->intrinsicId);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

} //namespace nlang
