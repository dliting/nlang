/*---
    EmitExprMemberCall.cpp — 成员表达式方法调用发射：stdlib/枚举/类/接口方法调用、内建泛型装箱计划与方法分派。
    从 EmitExprMember.cpp 拆出；字符串/toString 族拆至 EmitExprMemberString.cpp（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/langservice/SymbolIndex.h>
#include <cassert>
#include <map>
#include <vector>
#include <unordered_set>

namespace nlang {

//Phase 13 Step 2: bound method reference (c.foo / o.run in a
//value position) — the resolver bound the member's EvalDataType
//to the func declaration while Field() stayed the SnFunction
//(mirrors the bare-name OP_MakeFunc arm). VALUE POSITION ONLY:
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
    std::vector<OutSpill> outSpills;
    //Enum methods reject default and out formals at
    //declaration (D4) — the binding path emits exactly
    //the actual arguments.
    EmitCallArgs(invoke, callee, emitter, /*slotBase=*/1,
                 /*pArgPlans=*/nullptr,
                 /*thisSlot=*/resultOffset, &outSpills);
    if (!outSpills.empty())
        throw std::runtime_error(
            "NLang backend: out argument on enum method "
            "call");
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
        std::vector<OutSpill> outSpills;
        EmitCallArgs(invoke, invoke.Callee(), emitter, /*slotBase=*/1,
                     /*pArgPlans=*/nullptr, /*thisSlot=*/resultOffset,
                     &outSpills);
        //EmitCallArgs now copies `this` to claim[0] and bulk-copies
        //to callParamBase[0] — no separate this-copy needed here.
        //Phase 9e: interface dispatch is name-based (virtual) —
        //out args are resolver-rejected; spills here are internal.
        if (!outSpills.empty())
            throw std::runtime_error(
                "NLang backend: out argument on interface call");
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
//the generic-plan + dispatch machinery.
void VmBackend::EmitMemberClassMethodCall(SnInvokeExpr& invoke,
                                          SnClassDecl& classDecl,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
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
        EmitMemberDelegateInvoke(invoke, classDecl, emitter, resultOffset);
        return;
    }
    EmitMemberGenericMethodCall(invoke, classDecl, emitter, resultOffset);
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
                                         uint16_t resultOffset) {
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
    std::vector<OutSpill> delegateOutSpills;
    EmitCallArgs(invoke, nullptr, emitter, /*slotBase=*/0,
                 /*pArgPlans=*/nullptr,
                 /*thisSlot=*/UINT16_MAX,
                 &delegateOutSpills);
    EmitDelegateDispatch(emitter, calleeSlot, resultOffset,
                         delegateOutSpills);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//User/built-in class method call: plan the Phase 8e-4 boxing for generic
//instantiations, stage the arguments, then dispatch.
void VmBackend::EmitMemberGenericMethodCall(SnInvokeExpr& invoke,
                                            SnClassDecl& classDecl,
                                            BytecodeEmitter& emitter,
                                            uint16_t resultOffset) {
    std::map<uint16_t, ArgBoxPlan> argPlans;
    bool returnsBoxed = false;
    uint8_t returnTag = 0;
    if (classDecl.IsGenericInstantiation()) {
        PlanGenericMethodBoxing(classDecl, invoke.CalleeName(), argPlans,
                                returnsBoxed, returnTag);
    }
    auto* callee = invoke.Callee();
    //Phase 9e: out-argument spills collected by EmitCallArgs.
    std::vector<OutSpill> outSpills;
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
                 &argPlans, /*thisSlot=*/resultOffset,
                 &outSpills);
    //EmitCallArgs now copies `this` to claim[0] and bulk-copies
    //to callParamBase[0] — no separate this-copy needed here.
    EmitMemberMethodDispatch(invoke, callee, returnsBoxed, returnTag,
                             outSpills, emitter, resultOffset);
}

//The template's non-static methods of this name (the declared
//templates declare no method overloads today; the boxing derivation
//below stays correct for a future overload set by intersecting).
static void CollectTemplateMethods(SnClassDecl& tmpl,
                                   const std::string& methodName,
                                   std::vector<SnFunction*>& out)
{
    for (auto& member : tmpl.Members())
        if (member.Kind() == NK_Function
            && member.Name() == methodName
            && !member.ContainFlags(NF_Static))
            out.push_back(static_cast<SnFunction*>(&member));
}

//The placeholder every same-name method puts at one formal position —
//null on any absence or disagreement (the resolver's overload-
//intersection rule: a mixed position is not element-typed). ParamList
//is iterable but not indexable, so the position walks each list.
static SnField* UnanimousFormalPlaceholder(
    const std::vector<SnFunction*>& methods, size_t iParam,
    const GenericInstanceRegistry& generics)
{
    SnField* pUnanimous = nullptr;
    for (auto* pFn : methods)
    {
        SnFieldExpr* pType = nullptr;
        size_t i = 0;
        for (auto& param : pFn->Params())
            if (i++ == iParam) { pType = param.Type(); break; }
        SnField* pHere = pType ? pType->Field() : nullptr;
        if (!pHere || !generics.IsPlaceholder(pHere)
            || (pUnanimous && pUnanimous != pHere))
            return nullptr;
        pUnanimous = pHere;
    }
    return pUnanimous;
}

//The placeholder every same-name method returns — null on any
//disagreement (the same unanimity the formal side requires).
static SnField* UnanimousReturnPlaceholder(
    const std::vector<SnFunction*>& methods,
    const GenericInstanceRegistry& generics)
{
    SnFieldExpr* pFirst = methods.front()->ReturnType();
    SnField* pLeaf = pFirst ? pFirst->Field() : nullptr;
    if (!pLeaf || !generics.IsPlaceholder(pLeaf))
        return nullptr;
    for (size_t i = 1; i < methods.size(); ++i)
    {
        SnFieldExpr* pOther = methods[i]->ReturnType();
        if (!pOther || pOther->Field() != pLeaf)
            return nullptr;
    }
    return pLeaf;
}

//The boxing tag for one placeholder-typed slot: the type argument the
//placeholder binds to decides the tag. Anything else (a null leaf —
//overload disagreement or absence, a non-placeholder binding, a
//malformed arg index, a class/struct or array-typed argument) reports
//not-primitive and never boxes.
VmBackend::BoxingTagResult VmBackend::PlaceholderBoxingTag(
    const GenericInstanceRegistry& declaredGenerics, SnField* pPlaceholder,
    const std::vector<SnField*>& typeArgs)
{
    if (!pPlaceholder || !declaredGenerics.IsPlaceholder(pPlaceholder))
        return {0, false};
    const size_t argIdx = declaredGenerics.PlaceholderIndex(pPlaceholder);
    if (argIdx >= typeArgs.size())
        return {0, false};
    return BoxingTagFor(typeArgs[argIdx]);
}

//Boxed-element generic names (intrinsic ABI property): List, Dict and
//Channel store every element in erased side tables, so a primitive
//type argument must ride OP_Box across the call boundary. Future and
//Promise are absent — their values ride the context result slot raw.
static const char* const kBoxedElementGenericNames[] = {
    kBuiltinListTypeName, kBuiltinDictTypeName, kBuiltinChannelTypeName,
};

//Phase-2 D6: the boxing plan reads the DECLARED signatures. The
//template method's placeholder-typed formals decide which argument
//slots are element storage; the instance's type arguments decide each
//slot's tag (add/indexOf/contains at slot 1, set's value at slot 2,
//get/recv on the return side — the same plan the three retired
//(baseName, method) tables encoded, now expressed by
//stdlib/builtins.n). Class/struct type arguments never box:
//BoxingTagFor returns isPrimitive=false and the raw heap handle
//passes through, as do array-typed slots (the interned token is not
//a primitive).
void VmBackend::PlanGenericMethodBoxing(SnClassDecl& classDecl,
                                        const std::string& methodName,
                                        std::map<uint16_t, ArgBoxPlan>& argPlans,
                                        bool& returnsBoxed,
                                        uint8_t& returnTag) {
    const auto& baseName = classDecl.BaseName();
    bool bBoxedElementGeneric = false;
    for (const char* szName : kBoxedElementGenericNames)
        if (baseName == szName) { bBoxedElementGeneric = true; break; }
    if (!bBoxedElementGeneric || !m_pRegistry)
        return;
    auto& declaredGenerics = m_pRegistry->DeclaredGenerics();
    SnClassDecl* pTemplate = declaredGenerics.InstanceOf(&classDecl);
    if (!pTemplate)
        return;
    std::vector<SnFunction*> methods;
    CollectTemplateMethods(*pTemplate, methodName, methods);
    if (methods.empty())
        return;
    const auto& typeArgs = classDecl.GenericTypeArgs();
    //Slot 0 stages the receiver; formal position i boxes slot i+1.
    size_t maxParams = 0;
    for (auto* pFn : methods)
        if (pFn->Params().size() > maxParams)
            maxParams = pFn->Params().size();
    for (size_t iParam = 0; iParam < maxParams; ++iParam)
    {
        BoxingTagResult tag = PlaceholderBoxingTag(declaredGenerics,
            UnanimousFormalPlaceholder(methods, iParam, declaredGenerics),
            typeArgs);
        if (tag.isPrimitive)
            argPlans[static_cast<uint16_t>(iParam + 1)] = {tag.tag, true};
    }
    BoxingTagResult retTag = PlaceholderBoxingTag(declaredGenerics,
        UnanimousReturnPlaceholder(methods, declaredGenerics), typeArgs);
    if (retTag.isPrimitive)
    {
        returnsBoxed = true;
        returnTag = retTag.tag;
    }
}

//Class method dispatch: virtual name-based, direct by function index
//(with/without out spills), or builtin/intrinsic name-based; then the
//return unbox, result assign, out spills and ParaEnd.
void VmBackend::EmitMemberMethodDispatch(const SnInvokeExpr& invoke,
                                         SnFunction* callee,
                                         bool returnsBoxed, uint8_t returnTag,
                                         const std::vector<OutSpill>& outSpills,
                                         BytecodeEmitter& emitter,
                                         uint16_t resultOffset) {
    bool isVirtual = callee && callee->ContainFlags(NF_Virtual);
    if (isVirtual) {
        //Virtual method dispatch — name-based lookup at runtime
        //Phase 9e: out args are resolver-rejected on virtual
        //methods; spills here would be silently lost.
        if (!outSpills.empty())
            throw std::runtime_error(
                "NLang backend: out argument on virtual call");
        uint16_t nameIdx = AddStringConstant(callee->Name());
        emitter.Emit(OpCode::OP_CallMethod);
        emitter.EmitUint16(nameIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
    } else if (callee) {
        //Non-virtual (final) method — direct call by function index
        EmitMemberDirectMethodCall(callee, outSpills, emitter);
    } else {
        //Phase 8e-1: no AST callee — built-in class methods and inherited
        //Object protocol methods (shape note in the helper).
        EmitMemberBuiltinMethodCall(invoke, outSpills, emitter);
    }
    //Phase 8e-3/8e-4: unbox Get() return for primitive T/V.
    if (returnsBoxed) {
        emitter.Emit(OpCode::OP_Unbox);
        emitter.EmitByte(returnTag);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    //Phase 9e: out spills LAST — OP_VarLocal clobbers pResult,
    //so the return value (and any unbox) must complete first.
    if (!outSpills.empty())
        EmitOutSpills(outSpills, emitter);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Direct-call arm: non-virtual (final) method, dispatched by function
//index; OP_CallMethodDirectOut + mask when out arguments are present.
void VmBackend::EmitMemberDirectMethodCall(SnFunction* callee,
                                           const std::vector<OutSpill>& outSpills,
                                           BytecodeEmitter& emitter) {
    //Per-unit: a cross-unit method calls through an import placeholder;
    //own ones keep the registration-order index. The own-miss throw
    //inside FunctionSlotFor covers the Round-12 case (a resolver-bound
    //method with no body registers no record — emitting a stale-frame
    //read would be silent wrong code).
    const uint16_t funcIdx = static_cast<uint16_t>(
        FunctionSlotFor(*callee));
    if (!outSpills.empty()) {
        emitter.Emit(OpCode::OP_CallMethodDirectOut);
        emitter.EmitUint16(funcIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.EmitInt32(static_cast<int32_t>(
            BuildOutMask(outSpills)));
    } else {
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(funcIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
    }
}

//Phase 8e-1: no AST callee. Covers two cases:
//  (a) Builtin class methods (ByteStream/FileStream/List/Dict) —
//      the synthesized SnClassDecl has no method members.
//  (b) User-class calls to inherited Object protocol methods
//      (Equals/GetHashCode) — no AST override exists, so
//      callee is null. The VM walks superClassIdx to find
//      Object's intrinsic stub and short-circuits to
//      ExecuteIntrinsic.
//Both paths dispatch by name via OP_CallMethod. Phase 9e:
//out args cannot bind here (no formals) — internal error.
void VmBackend::EmitMemberBuiltinMethodCall(const SnInvokeExpr& invoke,
                                            const std::vector<OutSpill>& outSpills,
                                            BytecodeEmitter& emitter) {
    if (!outSpills.empty())
        throw std::runtime_error(
            "NLang backend: out argument on builtin call");
    uint16_t nameIdx = AddStringConstant(invoke.CalleeName());
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
}

} //namespace nlang
