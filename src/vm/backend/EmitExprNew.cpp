/*---
    EmitExprNew.cpp — 构造表达式发射：new/new array（初始化列表在 EmitExprInitList.cpp）。
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
#include <vector>

namespace nlang {

void VmBackend::Access(SnNewExpr& expr) {
    auto& newExpr = static_cast<SnNewExpr&>(expr);
    EmitNewObject(newExpr.ClassDecl(), newExpr.Args(), newExpr.ClassName(),
                  m_resultOffset);
}

//Shared new-object emission: `new Foo(args)` and the 0.9.0 ctor-invoke
//twin (`Foo(args)` without new) land here. Both failure modes are
//internal invariant breaks (Round-12) — codegen only runs when the
//front-end saw no errors.
void VmBackend::EmitNewObject(SnClassDecl* pClassDecl,
                              SnExpressionList& ctorArgs,
                              SnExpression* pSkipArg, uint16_t resultOffset) {
    if (!pClassDecl) {
        //Round-12: the resolver failed to bind a class declaration to
        //this construction.
        throw std::runtime_error(
            "NLang backend: new expression without a bound class declaration");
    }
    //Per-unit: own/builtin classes resolve through the table lookup;
    //a cross-unit class becomes an import placeholder slot (ClassSlotFor).
    //KeyOf keeps the erased builtin key ("List") for a generic
    //instantiation and the bare name for ownerless builtins, so the
    //own branch of the slot helper covers every ownerless shape.
    //Own/builtin records carry the ctor index in the table; a cross-unit
    //placeholder discovers its ctor in the AST (CtorSlotFor).
    const uint16_t classIdx =
        static_cast<uint16_t>(ClassSlotFor(*pClassDecl));
    const uint16_t ctorIdx = CtorSlotFor(*pClassDecl, classIdx);
    if (ctorIdx != 0xFFFF) {
        EmitCtorInvocation(pClassDecl, ctorArgs, pSkipArg, *m_pCurrEmitter,
                           classIdx, ctorIdx, resultOffset);
        return;
    }

    BytecodeEmitter& emitter = *m_pCurrEmitter;
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(classIdx);
}

//The class has a ctor — claim {this, formals...} in evalArea, emit one
//FormalBinding per formal (caller args first, then the defaults of the
//omitted trailing formals — P3), OP_New into resultOffset, bulk-copy the
//claim to callParamBase, then OP_CallMethodDirect on the ctor. pSkipArg
//is the class-name child the NewExpr arg view carries (skipped by
//identity; null for the invoke twin). The resolver rejects named/out
//args and range-checks the count, so every supplied entry is a
//positional value expression.
void VmBackend::EmitCtorInvocation(SnClassDecl* pClassDecl,
                                   SnExpressionList& ctorArgs,
                                   SnExpression* pSkipArg,
                                   BytecodeEmitter& emitter, uint16_t classIdx,
                                   uint16_t ctorIdx, uint16_t resultOffset) {
    std::vector<SnExpression*> callerArgs;
    for (auto& param : ctorArgs) {
        if (&param == pSkipArg) continue;
        callerArgs.push_back(&param);
    }
    //Defaults-aware bindings (P3): builtins/generics carry no AST ctor,
    //so BuildCtorBindings returns the caller-arg count and leaves the
    //bindings empty — EmitCtorCallBindings then falls back to the
    //pre-P3 positional shape. No kMaxFuncParams re-check: the
    //declaration-time cap already bounds a ctor's formal count.
    std::vector<FormalBinding> bindings;
    SnFunction* pCtorFn = FindCtorFunction(pClassDecl);
    const size_t totalFormals = BuildCtorBindings(pCtorFn, callerArgs,
                                                  bindings);
    //Recursion guard shared with the function/method default emission
    //(see BindingsEmitDefaults for the arm-only rule).
    DefaultEmitGuard defGuard{*this, pCtorFn,
                              BindingsEmitDefaults(bindings)};

    const uint16_t n = static_cast<uint16_t>(1 + totalFormals);  //this + formals

    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();

    //Bindings → claim[1..N]. thisSlot is UINT16_MAX: the object under
    //construction does not exist yet, so a ctor default must not
    //reference `this`. Nested calls in bindings claim deeper slices —
    //they bulk-copy to callParamBase but that's fine; our this/formals
    //live in evalArea, not callParamBase.
    EmitCtorCallBindings(bindings, callerArgs, claimBase, emitter,
                         UINT16_MAX);
    //Allocate to resultOffset (no conflict with bindings — they're in
    //evalArea), stage the object as claim[0], and invoke the ctor.
    EmitCtorCallTail(resultOffset, claimBase, n, classIdx, ctorIdx,
                     emitter);
    //claim releases on scope exit.
}

    // This expression - reads the implicit first parameter
void VmBackend::Access(SnNewArrayExpr& expr) {
    auto& newArr = static_cast<SnNewArrayExpr&>(expr);
    EmitNewArray(newArr.ElementType()->Field(), *newArr.Size(),
                 m_resultOffset);
}

//Shared new-array emission: `new T[size]` and the 0.9.0 type-subscript
//twin (`T[size]` in expression position) land here.
void VmBackend::EmitNewArray(SnField* pElemType, SnExpression& sizeExpr,
                             uint16_t resultOffset) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    //Register the array type (idempotent)
    uint16_t arrayTypeIdx = RegisterArrayType(pElemType);
    //Size staging: EvalAreaClaim, never a temp. A temp staging slot
    //lets a nested binary (`1 + use(make(1))`) park its LEFT operand
    //there while a struct-by-value argument deep-copies through
    //tempSlot (EmitBinding CopyStruct scratch) — silently corrupting
    //the parked value. Claim slots are exclusive to this expression.
    //ExprPeakDepth's NewArrayExpr case tracks the claim=1.
    EvalAreaClaim sizeClaim(*this, 1);
    uint16_t sizeSlot = sizeClaim.base();
    EmitExpression(sizeExpr, emitter, sizeSlot);
    emitter.Emit(OpCode::OP_AllocArray);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(sizeSlot);
}
} //namespace nlang
