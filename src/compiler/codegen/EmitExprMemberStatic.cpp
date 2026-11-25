/*---
    EmitExprMemberStatic.cpp — 成员表达式静态调用族发射：声明类静态
    方法（Util.twice(21)，0.9.10）与内建泛型类型名上的静态面
    （Future.selectAny）。两者都无接收者——外层限定符从不作为值发射，
    实参自槽 0 起。从 EmitExprMemberCall.cpp 拆出（500 行预算）。
---*/
#include "VmBackend.h"
#include "builder/BuiltinNames.h"
#include "builder/ModuleRegistry.h"
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
//index like the final-method arm. A callee-less invoke with a func-typed
//static field is the delegate flavor of the same receiverless shape
//(Cfg.dbl(21) — a lambda parked in a static initializer).
bool VmBackend::EmitMemberStaticCall(SnMemberExpr& member,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr))
        return false;
    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
    auto* callee = invoke.Callee();
    if (!callee)
        return EmitMemberStaticDelegateCall(invoke, emitter, resultOffset);
    if (!callee->ContainFlags(NF_Static))
        return false;
    auto& generics = m_pRegistry->DeclaredGenerics();
    //0.13.1a Task 2: an inferred template-row static — the resolver
    //bound the clone member of a created instantiation (the registry's
    //fill-time clone maps hold the pair). OP_CallMethodDirect (the
    //ordinary arm below) has no descriptor operand, so the switch is
    //mandatory, not stylistic.
    if (generics.RecordOwnerOfMember(callee))
    {
        EmitInferredTemplateRowStaticCall(invoke, *callee, emitter,
                                          resultOffset);
        return true;
    }
    //First active use gates the callee's declaring class (D2/D4) —
    //before the arguments stage.
    EmitStaticFieldGate(callee, emitter);
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX);
    EmitMemberDirectMethodCall(callee, emitter);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

//Delegate flavor of the receiverless static call: the callee name bound
//to a func-typed STATIC field, so the handle loads straight from the
//module slot — gate the declaring class's initializer first (D2), claim
//the callee slot BEFORE the args so nested emissions cannot clobber it
//(the EmitMemberDelegateInvoke contract), stage user args from slot 0,
//dispatch by delegate. False when the shape is not this one (dispatch
//continues to the receiver-bearing arms).
bool VmBackend::EmitMemberStaticDelegateCall(SnInvokeExpr& invoke,
                                             BytecodeEmitter& emitter,
                                             uint16_t resultOffset) {
    auto* field = invoke.Field();
    if (!(field && field->Kind() == NK_ClassField
          && field->ContainFlags(NF_Static)))
        return false;
    auto target = ResolveBareIdentifier(field);
    if (target.kind != BareIdTarget::StaticSlot)
        return false;  //defensive: a static class field always slots
    EmitStaticFieldGate(field, emitter);
    //K2 slice 5: the parked delegate handle is a bound-func record
    //index — always the heap domain.
    EvalAreaClaim calleeClaim(*this, 1, ESC_HeapIndex);
    uint16_t calleeSlot = calleeClaim.base();
    emitter.Emit(OpCode::OP_LoadStatic);
    emitter.EmitUint16(calleeSlot);
    emitter.EmitUint16(target.slotIdx);
    EmitCallArgs(invoke, nullptr, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX);
    //A raw &-return binds the pair itself; a value read normalizes
    //through the key's return deref tag (same contract as the member
    //delegate arm).
    const uint8_t refDerefKind = DelegateReturnDerefKind(
        DelegateFuncTypeOfField(invoke.Field()));
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset, refDerefKind);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

//0.13.1a Task 2: the inferred template-row static arm. First active use
//gates the RECORD OWNER's initializer — the template row owns the
//compiled records; a clone carries no Parent() of its own. The frame
//descriptor cell rides OP_CallStatic's operand: the instantiation's
//descriptor row, a composition record row when a caller type parameter rides an
//argument (kInstDescNone for a clone the registry never paired —
//defensive; the resolver's arm always pairs).
void VmBackend::EmitInferredTemplateRowStaticCall(SnInvokeExpr& invoke,
                                                  SnFunction& callee,
                                                  BytecodeEmitter& emitter,
                                                  uint16_t resultOffset) {
    auto& generics = m_pRegistry->DeclaredGenerics();
    SnField* pOwnerMember = generics.RecordOwnerOfMember(&callee);
    EmitStaticFieldGate(pOwnerMember, emitter);
    EmitCallArgs(invoke, &callee, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX);
    auto* pInst = generics.InstantiationOfClone(&callee);
    uint16_t descIdx = pInst ? DescriptorIndexFor(*pInst) : kInstDescNone;
    if (!pInst)
    {
        //0.13.1c Task 8: the interface twin — a clone static's owner may
        //be a generic INTERFACE instantiation (`Pair<int,string>.echo`);
        //the class channel answers class clones, the interface channel
        //interface clones (each adopt names its own kind, never aliased).
        auto* pIfaceInst = generics.InstantiationOfInterfaceClone(&callee);
        if (pIfaceInst)
            descIdx = DescriptorIndexFor(*pIfaceInst);
    }
    EmitFreeFunctionCall(static_cast<int>(FunctionSlotFor(callee)), emitter,
                         resultOffset, descIdx);
    emitter.Emit(OpCode::OP_ParaEnd);
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
//Task 7 (D11): a runtime interface constant reaches here past the
//constant-read phase (only folded constants were consumed there) —
//its read gates the INTERFACE's lazy initializer (kind 1) and loads
//the slot the initializer stores.
bool VmBackend::EmitMemberStaticFieldRead(SnMemberExpr& member,
                                          SnField* field,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    if (!(field && field->Kind() == NK_ClassField
          && field->ContainFlags(NF_Static)))
        return false;
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_IdentifierExpr))
        return false;   //invoke shapes belong to the call arms
    if (IsInterfaceConstantField(*field)) {
        //Folded constants never reach a slot — a map miss with no
        //producer rider means the constant-read phase already emitted
        //the literal. An imported runtime constant (Task 8) is a map
        //miss BY CONSTRUCTION (the row lives in the producer's image)
        //and carries the rider instead; a same-build foreign unit (an
        //inlined library source) carries no wire rider but the shadow
        //archive replays exactly what the producer created (0xFFFF =
        //the producer folded it too). Runtime channel = any of the
        //three; it proceeds to gate+load and creates its placeholder
        //slot below.
        auto* pConstField = static_cast<SnClassField*>(field);
        const bool ownSlot = m_staticSlotByField.find(pConstField)
                             != m_staticSlotByField.end();
        const bool runtimeChannel = ownSlot
            || EffectiveProducerSlotIdx(*pConstField) != 0xFFFF;
        if (!runtimeChannel)
            return false;
        auto* owner =
            static_cast<SnInterfaceDecl*>(field->Parent());
        EmitInterfaceInitGate(*owner, emitter);
        emitter.Emit(OpCode::OP_LoadStatic);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(StaticSlotIndexOf(
            static_cast<SnClassField&>(*field)));
        return true;
    }
    //First active use gates the DECLARING class's initializer (D2/D4).
    EmitStaticFieldGate(field, emitter);
    emitter.Emit(OpCode::OP_LoadStatic);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(StaticSlotIndexOf(
        static_cast<SnClassField&>(*field)));
    return true;
}

} //namespace nlang
