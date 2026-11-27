/*--- EmitExprDelegate.cpp — the delegate invoke emit family: callee
    handle staging (named-shape storage arms and the postfix callee
    expression), argument staging handoff and the OP_CallDelegate
    dispatch. Split from EmitExprCore.cpp by the source-size guard
    (v0.13.13a), zero behavior change; per-concern-TU precedent
    EmitCaptureFrame.cpp.
---*/
#include "VmBackend.h"
#include "builder/FieldInitializerChecks.hpp"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <cassert>
#include <map>
#include <unordered_set>
#include <nlang/runtime/StackDump.h>

namespace nlang {

//Delegate invoke arm: materialize the callee handle into a scratch
//evalArea slot, stage the actual arguments, then dispatch through
//EmitDelegateDispatch. Callee first, args second (v0.13.13a D-3): a
//call nested in a postfix callee expression (a container subscript,
//a chained call) stages its own arguments at callParamBase, so
//staging the caller's actuals before evaluating the callee let the
//nested call clobber them. The claim keeps the handle out of any
//nested staging either way, and callee-first is also the
//left-to-right evaluation order of the mainstream postfix-call
//spelling.
//Task 8: a raw &-return binds the pair itself (binding targets, nested
//ref-calls); a value read normalizes through the key's return deref tag.
void VmBackend::EmitDelegateInvoke(const SnInvokeExpr& invoke,
                                   BytecodeEmitter& emitter,
                                   uint16_t resultOffset,
                                   bool bRawRefResult) {
    EvalAreaClaim calleeClaim(*this, 1);
    uint16_t calleeSlot = calleeClaim.base();
    //v0.13.13a D-3: the postfix shape carries the callee as an
    //expression — evaluate it into the staged slot (its value IS the
    //func handle, the resolver admitted only func-typed callees); the
    //named shape loads the value field's handle.
    if (auto* pCalleeExpr = invoke.CalleeExpr())
        EmitExpression(*pCalleeExpr, emitter, calleeSlot);
    else {
        auto target = ResolveBareIdentifier(invoke.Field());
        EmitDelegateCalleeLoad(invoke, target, calleeSlot, emitter);
    }
    //Binding-aware positional staging at callParamBase (the
    //OP_CallDelegate read position), after the handle is parked.
    EmitCallArgs(invoke, nullptr, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr, /*thisSlot=*/UINT16_MAX);
    const uint8_t refDerefKind = bRawRefResult ? kNoRefDeref
        : DelegateReturnDerefKind(DelegateFuncTypeOfInvoke(invoke));
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset,
        refDerefKind);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//The delegate callee's handle load, one arm per resolved storage kind.
//The capture arm carries its own rejection (a flavored capture's storage
//is not a raw handle — ledger ruling); the fall-through throw is the
//invariant break for any unmatched kind.
void VmBackend::EmitDelegateCalleeLoad(const SnInvokeExpr& invoke,
                                       const BareIdTarget& target,
                                       uint16_t calleeSlot,
                                       BytecodeEmitter& emitter) {
    if (target.kind == BareIdTarget::Local) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(calleeSlot);
    } else if (target.kind == BareIdTarget::DisplayField
        || target.kind == BareIdTarget::CaptureField) {
        //0.13.10 Task 6: a func-typed capture as delegate callee. The
        //delegate handle rides the display field cell: chase the record,
        //load the cell.
        auto* pField = invoke.Field();
        if (pField && (pField->ContainFlags(NF_Promoted)
            || pField->ContainFlags(NF_Ref)))
            throw std::runtime_error(
                "NLang backend: promoted/reference capture as a "
                "delegate callee is not supported: "
                + invoke.CalleeName());
        EmitCaptureCellLoad(calleeSlot, target, pField, emitter);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else if (target.kind == BareIdTarget::StaticSlot) {
        //I3 tail: a func-typed STATIC field as delegate callee (the bare
        //in-class spelling of Cfg.dbl) — gate the declaring class, load
        //the handle from the module slot; no receiver exists.
        EmitStaticFieldGate(invoke.Field(), emitter);
        emitter.Emit(OpCode::OP_LoadStatic);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(target.slotIdx);
    } else if (target.kind == BareIdTarget::ThisField) {
        //Implicit this.<field> — same load shape as the
        //identifier arm's ThisField branch.
        EmitReceiverLoad(calleeSlot, emitter);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(calleeSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    } else {
        throw std::runtime_error(
            "NLang backend: delegate callee has no codegen "
            "binding: " + invoke.CalleeName());
    }
}

//Delegate dispatch: OP_CallDelegate. A ref-returning key parks a raw
//pair — deref in place for a value consumer.
void VmBackend::EmitDelegateDispatch(BytecodeEmitter& emitter,
                                     uint16_t calleeSlot,
                                     uint16_t resultOffset,
                                     uint8_t refDerefKind) {
    emitter.Emit(OpCode::OP_CallDelegate);
    emitter.EmitUint16(calleeSlot);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    if (refDerefKind != kNoRefDeref)
        EmitRefDerefInPlace(refDerefKind, resultOffset, emitter);
}

} //namespace nlang
