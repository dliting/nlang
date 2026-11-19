/*---
    EmitExprMemberCall.cpp — 成员表达式方法调用发射：stdlib/枚举/类/接口方法调用与方法分派。
    从 EmitExprMember.cpp 拆出；字符串/toString 族拆至 EmitExprMemberString.cpp（2026-09-25）；
    泛型装箱规划拆至 EmitGenericBoxing.cpp（0.13.1a Task 5 尺寸守卫拆分）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <map>
#include <vector>
#include <unordered_set>

namespace nlang {

//Phase 13 Step 2: bound method reference (c.foo / o.run in a
//value position) — the resolver bound the member's EvalDataType
//to the func declaration while Field() stayed the SnFunction
//(mirrors the unqualified-name OP_MakeFunc arm). VALUE POSITION ONLY:
//the inner node must be an identifier (c.foo). A call shape
//(c.foo(), inner == invoke) resolves to the same func-typed
//member but must fall through to the invoke emission below —
//binding it here would emit a bound-reference opcode over a
//receiver that no expression ever produced (review C1: a
//module-qualified func-returning call crashed with "null
//receiver in method reference"). Receiver-first: emit the
//receiver to resultOffset, refresh pResult, then the bind
//opcode reads the receiver from pResult and writes the handle
//there. Form selection matches the direct-call codegen decision:
//virtual methods and interface declarations dispatch by name,
//everything else binds the static function index. The executor's
//bind-time null guard covers null receivers.
void VmBackend::EmitMemberFuncHandleRef(SnMemberExpr& member, SnField* field,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    auto* method = static_cast<SnFunction*>(field);
    bool dispatchesByName = method->ContainFlags(NF_Virtual)
        || (method->Parent()
            && method->Parent()->Kind() == NK_InterfaceDecl);
    EmitExpression(*member.Outer(), emitter, resultOffset);
    EmitPResultRefresh(emitter, resultOffset);
    if (dispatchesByName) {
        uint16_t nameIdx = AddStringConstant(method->Name());
        emitter.Emit(OpCode::OP_MakeVFunc);
        emitter.EmitUint16(nameIdx);
    } else {
        //Per-unit: a cross-unit method binds an import-placeholder
        //index; own methods keep the registration-order index.
        emitter.Emit(OpCode::OP_MakeBoundFunc);
        emitter.EmitUint16(static_cast<uint16_t>(
            FunctionSlotFor(*method)));
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//v2.2: receiverless function reference (lib.seven or a static method in
//value position) — the bare OP_MakeFunc form of the identifier arm (no
//receiver); a cross-unit reference becomes an import placeholder the
//load-time linker resolves. A receiver-bound method form never reaches
//here (the dispatch splits the shapes before calling). The byte stream
//is exactly the identifier arm's, so the body delegates to it.
void VmBackend::EmitReceiverlessFuncHandleRef(SnField& func,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    EmitIdentifierFuncHandleRead(emitter, resultOffset, &func);
}

//Phase 12: user-defined enum method call (c.rank(),
//Color.Blue.rank(), this.weight()). Detection is by the resolver
//binding: invoke.Field() is the SnFunction whose parent is the
//enum decl — both receiver shapes bind identically. The receiver
//is the enum VALUE (an int32 slot), so unlike the class path
//there is no null check and no boxing; the claim shape is the
//same receiver-first form with thisSlot=resultOffset. The builtin
//toString dispatch below stays reachable: user methods of that
//name are rejected at declaration (D5), so a bound enum-method
//callee is never toString.
//Returns true when the call was emitted.
bool VmBackend::EmitMemberEnumMethodCall(SnMemberExpr& member,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    auto* inner = member.Inner();
    if (!(inner && inner->Kind() == NK_InvokeExpr))
        return false;
    auto& invoke = static_cast<SnInvokeExpr&>(*inner);
    auto* callee = invoke.Callee();
    if (!(callee && callee->Parent()
        && callee->Parent()->Kind() == NK_EnumDecl))
        return false;
    //Evaluate the receiver (enum value) into resultOffset.
    EmitExpression(*member.Outer(), emitter, resultOffset);
    //Enum methods reject default formals at declaration (D4) —
    //the binding path emits exactly the actual arguments.
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                 /*pArgPlans=*/nullptr,
                 /*thisSlot=*/resultOffset);
    //Per-unit: a cross-unit enum method calls through an import
    //placeholder; own ones keep the registration-order index (the own
    //miss throws inside FunctionSlotFor — a body-less enum method
    //declaration registers no record).
    emitter.Emit(OpCode::OP_CallMethodDirect);
    emitter.EmitUint16(static_cast<uint16_t>(FunctionSlotFor(*callee)));
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
    return true;
}

//Interface method call (e.g. p.Print()).
//At runtime, p holds a heap reference whose actual class is unknown
//at compile time. Always dispatch virtually by method name — the
//VM walks the runtime class's methodIndices via superClassIdx.
void VmBackend::EmitMemberInterfaceCall(SnMemberExpr& member,
                                        BytecodeEmitter& emitter,
                                        uint16_t resultOffset) {
    //Evaluate outer expression to resultOffset (gets heap index)
    EmitExpression(*member.Outer(), emitter, resultOffset);
    //Null check
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(resultOffset);
    auto* inner = member.Inner();
    if (inner && inner->Kind() == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(*inner);
        //Phase 9c: binding-aware arg emit; slot 0 reserved for `this`.
        //thisSlot=resultOffset so default-param `this.field` reads
        //the receiver from the outer-expression result slot.
        EmitCallArgs(invoke, invoke.Callee(), emitter, /*slotBase=*/1,
                     /*pArgPlans=*/nullptr, /*thisSlot=*/resultOffset);
        //EmitCallArgs now copies `this` to claim[0] and bulk-copies
        //to callParamBase[0] — no separate this-copy needed here.
        //Always virtual dispatch by name.
        auto* callee = invoke.Callee();
        //Round-12: no callee means the resolver resolved the call
        //without binding a method — emitting nothing here would read
        //stale frame memory as the "result". Fail the build instead.
        if (!callee)
            throw std::runtime_error(
                "NLang backend: interface call without a bound method: "
                + invoke.CalleeName());
        uint16_t nameIdx = AddStringConstant(callee->Name());
        emitter.Emit(OpCode::OP_CallMethod);
        emitter.EmitUint16(nameIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        emitter.Emit(OpCode::OP_ParaEnd);
    }
}

//Class-receiver invoke arm: func-handle toString, delegate invoke, then
//the generic-plan + dispatch machinery. bRawRefResult (Task 6) threads
//to the dispatch tail: a &-returning callee's raw pair passes through.
void VmBackend::EmitMemberClassMethodCall(SnInvokeExpr& invoke,
                                          SnClassDecl& classDecl,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset,
                                          bool bRawRefResult) {
    //Phase 13: function-handle toString() — the only built-in
    //method on a func<...> receiver. The receiver is already
    //at resultOffset (loaded by the class-receiver prologue
    //above). Must precede the boxing-plan and method-table
    //machinery: a handle is a VM primitive (like an array),
    //func has no backing CompiledClass.
    if (classDecl.IsFuncType()
        && invoke.CalleeName() == "toString") {
        EmitPResultRefresh(emitter, resultOffset);
        emitter.Emit(OpCode::OP_Func_to_str);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(resultOffset);
        return;
    }
    //Phase 13 Step 2: delegate member invoke (obj.cb(x)) — shape note in
    //EmitMemberDelegateInvoke.
    if (IsDelegateInvoke(invoke)) {
        EmitMemberDelegateInvoke(invoke, classDecl, emitter, resultOffset,
            bRawRefResult);
        return;
    }
    EmitMemberGenericMethodCall(invoke, classDecl, emitter, resultOffset,
                                bRawRefResult);
}

//Phase 13 Step 2: delegate member invoke (obj.cb(x)) —
//the resolver bound the callee name to a func-typed FIELD
//of this class (the invoke's Field() is that field, not
//an SnFunction; D13). The receiver is already at
//resultOffset with the null check done. Unlike a method
//call, staging holds USER args only — slotBase 0, no this
//at slot 0. The handle is materialized into a scratch
//claim slot (claimed BEFORE the args so nested emissions
//cannot clobber it), matching the bare-form delegate arm.
void VmBackend::EmitMemberDelegateInvoke(SnInvokeExpr& invoke,
                                         SnClassDecl& classDecl,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset,
                                         bool bRawRefResult) {
    int off = FindClassFieldOffset(classDecl, invoke.CalleeName());
    if (off < 0) {
        throw std::runtime_error(
            "NLang backend: class field offset not found: "
            + invoke.CalleeName());
    }
    //K2 slice 5: the parked delegate handle is a bound-func record
    //index — always the heap domain.
    EvalAreaClaim calleeClaim(*this, 1, ESC_HeapIndex);
    uint16_t calleeSlot = calleeClaim.base();
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(calleeSlot);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(static_cast<uint16_t>(off));
    EmitCallArgs(invoke, nullptr, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr,
                 /*thisSlot=*/UINT16_MAX);
    //Task 8: a raw &-return binds the pair itself; a value read
    //normalizes through the key's return deref tag (same contract as
    //the bare-form delegate arm).
    const uint8_t refDerefKind = bRawRefResult ? kNoRefDeref
        : DelegateReturnDerefKind(DelegateFuncTypeOfField(invoke.Field()));
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset,
                         refDerefKind);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//User/built-in class method call: plan the Phase 8e-4 boxing for generic
//instantiations, stage the arguments, then dispatch.
void VmBackend::EmitMemberGenericMethodCall(SnInvokeExpr& invoke,
                                            SnClassDecl& classDecl,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset,
                                            bool bRawRefResult) {
    std::map<uint16_t, ArgBoxPlan> argPlans;
    ArgBoxPlan returnPlan;
    if (classDecl.IsGenericInstantiation()) {
        PlanGenericMethodBoxing(classDecl, invoke.CalleeName(), argPlans,
                                returnPlan);
    }
    auto* callee = invoke.Callee();
    //Phase 9c: evaluate args via the shared binding-aware
    //helper. Slot 0 is reserved for `this` (slotBase=1).
    //Boxing plans (for built-in generic class methods like
    //List<int>.add) are applied per-arg inside EmitCallArgs.
    //thisSlot=resultOffset: default-param expressions that
    //reference `this.field` resolve `this` to resultOffset
    //(the slot holding the receiver object). This avoids the
    //callParamBase nested-call clobber bug — copying `this`
    //to callParamBase[0] before emitting args would be
    //overwritten by any nested call inside the args.
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                 &argPlans, /*thisSlot=*/resultOffset);
    //EmitCallArgs now copies `this` to claim[0] and bulk-copies
    //to callParamBase[0] — no separate this-copy needed here.
    EmitMemberMethodDispatch(invoke, callee, returnPlan,
                             emitter, resultOffset,
                             bRawRefResult);
}

//Class method dispatch: virtual name-based, direct by function index,
//or builtin/intrinsic name-based; then the return normalization, result
//assign and ParaEnd. bRawRefResult (Task 6): a &-returning callee hands
//back a raw (record, cell) pair — skip the box normalization so a
//reference consumer receives the pair untouched.
void VmBackend::EmitMemberMethodDispatch(const SnInvokeExpr& invoke,
                                         SnFunction* callee,
                                         const ArgBoxPlan& returnPlan,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset,
                                         bool bRawRefResult) {
    bool isVirtual = callee && callee->ContainFlags(NF_Virtual);
    if (isVirtual) {
        //Virtual method dispatch — name-based lookup at runtime
        uint16_t nameIdx = AddStringConstant(callee->Name());
        emitter.Emit(OpCode::OP_CallMethod);
        emitter.EmitUint16(nameIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
    } else if (callee) {
        //Non-virtual (final) method — direct call by function index
        EmitMemberDirectMethodCall(callee, emitter);
    } else {
        //Phase 8e-1: no AST callee — built-in class methods and inherited
        //Object protocol methods (shape note in the helper).
        EmitMemberBuiltinMethodCall(invoke, emitter);
    }
    //Phase 8e-3/8e-4: unbox Get() return for primitive T/V. 0.13.1a
    //Task 5: when the receiver's type argument is the calling body's
    //own placeholder, the descriptor-dispatched variant lets the
    //executing frame decide (uniform-T-representation invariant).
    EmitMethodReturnNormalization(callee, returnPlan, bRawRefResult,
                                  resultOffset, emitter);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//The dispatch tail's return normalization (split for the 50-line
//function budget): unbox the container get() payload, or park the raw
//pair of a &-returning user method and deref it IN PLACE for a value
//consumer (scalar/string/enum returns only; struct/class returns ride
//the pair's low-half handle into every deep copy). A reference
//consumer takes the raw pair untouched. OP_LoadRef derefs any bindable
//pair — field-cell pairs included, unlike OP_Unbox.
void VmBackend::EmitMethodReturnNormalization(SnFunction* callee,
    const ArgBoxPlan& returnPlan, bool bRawRefResult,
    uint16_t resultOffset, BytecodeEmitter& emitter) {
    uint8_t derefKind = kNoRefDeref;
    if (bRawRefResult) {
        //reference consumer: no normalization
    } else if (returnPlan.needsBox) {
        EmitBoundaryUnbox(returnPlan, emitter);
    } else {
        derefKind = RefReturnDerefKind(callee);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    if (derefKind != kNoRefDeref)
        EmitRefDerefInPlace(derefKind, resultOffset, emitter);
}

//Direct-call arm: non-virtual (final) method, dispatched by function
//index.
void VmBackend::EmitMemberDirectMethodCall(SnFunction* callee,
                                           BytecodeEmitter& emitter) {
    //Per-unit: a cross-unit method calls through an import placeholder;
    //own ones keep the registration-order index. The own-miss throw
    //inside FunctionSlotFor covers the Round-12 case (a resolver-bound
    //method with no body registers no record — emitting a stale-frame
    //read would be silent wrong code).
    const uint16_t funcIdx = static_cast<uint16_t>(
        FunctionSlotFor(*callee));
    emitter.Emit(OpCode::OP_CallMethodDirect);
    emitter.EmitUint16(funcIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
}

//Phase 8e-1: no AST callee. Covers two cases:
//  (a) Builtin class methods (ByteStream/FileStream/List/Dict) —
//      the synthesized SnClassDecl has no method members.
//  (b) User-class calls to inherited Object protocol methods
//      (Equals/GetHashCode) — no AST override exists, so
//      callee is null. The VM walks superClassIdx to find
//      Object's intrinsic stub and short-circuits to
//      ExecuteIntrinsic.
//Both paths dispatch by name via OP_CallMethod.
void VmBackend::EmitMemberBuiltinMethodCall(const SnInvokeExpr& invoke,
                                            BytecodeEmitter& emitter) {
    uint16_t nameIdx = AddStringConstant(invoke.CalleeName());
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
}

} //namespace nlang
