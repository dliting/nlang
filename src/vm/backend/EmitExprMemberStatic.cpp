/*---
    EmitExprMemberStatic.cpp — 成员表达式静态调用族发射：声明类静态
    方法（Util.twice(21)，0.9.10）与内建泛型类型名上的静态面
    （Future.selectAny）。两者都无接收者——外层限定符从不作为值发射，
    实参自槽 0 起。从 EmitExprMemberCall.cpp 拆出（500 行预算）。
---*/
#include "VmBackend.h"
#include "builder/BuiltinNames.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <vector>

namespace nlang {

//0.9.10: type-name-qualified static call (Util.twice(21), I.stat()) —
//the resolver bound the inner invoke to an NF_Static callee. No receiver
//value exists, so the outer identifier is never emitted; arguments stage
//from slot 0 against a frame with no __this slot (AllocParamsAndDefaults
//skips the this slot for statics), and dispatch is direct by function
//index like the final-method arm.
bool VmBackend::EmitMemberStaticCall(SnMemberExpr& member,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr))
        return false;
    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
    auto* callee = invoke.Callee();
    if (!(callee && callee->ContainFlags(NF_Static)))
        return false;
    //First active use gates the callee's declaring class (D2/D4) —
    //before the arguments stage.
    EmitStaticFieldGate(callee, emitter);
    std::vector<OutSpill> outSpills;
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX,
                 &outSpills);
    EmitMemberDirectMethodCall(callee, outSpills, emitter);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    if (!outSpills.empty())
        EmitOutSpills(outSpills, emitter);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

//Static-face call (Future.selectAny(futures)): the type name as a call
//qualifier — no receiver exists. The resolver bound the outer
//identifier to the face qualifier singleton and left the invoke
//callee-less (the container-method family shape), so detection is the
//singleton's POINTER IDENTITY on the outer identifier's Field(); the
//emission reads the intrinsic id from the same face table row the
//resolver gated on. Args stage from claim slot 0 (the binding row's
//noReceiver ABI) and bulk-copy to callParamBase — the string-method
//shape minus the receiver. Walker symmetry: the MemberExpr branch
//walks the invoke with isMethodContext=true (+1 for `this`), which is
//exactly this claim's single slot — no walker change needed.
bool VmBackend::EmitMemberStaticFaceCall(SnMemberExpr& member,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr))
        return false;
    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
    if (invoke.Callee())   //a bound callee is a real call, not a face
        return false;
    auto* outer = member.Outer();
    if (!(outer && outer->Kind() == NK_IdentifierExpr))
        return false;
    const std::string& typeName =
        static_cast<SnIdentifierExpr*>(outer)->Name();
    SnClassDecl* singleton = FindBuiltinStaticFaceClassDecl(*m_pRegistry, typeName);
    if (!singleton
        || static_cast<SnFieldExpr*>(outer)->Field() != singleton)
        return false;
    const BuiltinStaticFace* face =
        FindBuiltinStaticFace(typeName, invoke.CalleeName());
    if (!face)
        return false;
    const uint16_t argCount = static_cast<uint16_t>(face->argCount);
    EvalAreaClaim claim(*this, argCount);
    uint16_t claimBase = claim.base();
    uint16_t paramIdx = 0;
    for (auto& param : invoke.Params()) {
        EmitExpression(param, emitter,
            claimBase + paramIdx * kFrameSlotBytes);
        ++paramIdx;
    }
    CopyClaimToCallParams(claimBase, argCount, emitter);
    emitter.Emit(OpCode::OP_CallIntrinsic);
    emitter.EmitUint16(face->intrinsicId);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

//v0.12.6: type-name-qualified static field read (Counter.total). The
//resolver's namespace filter guarantees the only member shape that
//reaches codegen with an NF_Static class field is the type-name form
//(a value receiver is rejected there, D3) — so the receiver is never
//emitted and the read is a plain module-slot load. The bound field is
//the DECLARING class's node for inherited statics (D4): the slot index
//follows the field, not the spelling.
bool VmBackend::EmitMemberStaticFieldRead(SnMemberExpr& member,
                                          SnField* field,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    if (!(field && field->Kind() == NK_ClassField
          && field->ContainFlags(NF_Static)
          //Interface constants are compile-time literals consumed by
          //the constant-read phase; they never load a slot here.
          && !IsInterfaceConstantField(*field)))
        return false;
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_IdentifierExpr))
        return false;   //invoke shapes belong to the call arms
    //First active use gates the DECLARING class's initializer (D2/D4).
    EmitStaticFieldGate(field, emitter);
    emitter.Emit(OpCode::OP_LoadStatic);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(StaticSlotIndexOf(
        static_cast<const SnClassField&>(*field)));
    return true;
}

} //namespace nlang
