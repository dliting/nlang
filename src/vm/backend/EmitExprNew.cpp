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
    //0.9.11 D2: exactly one of the two type slots is bound by the resolver
    //(the direct-init `S name(args);` spelling decomposes into a struct-
    //typed new expression); CtorFunction() carries the selected ctor.
    if (auto* pStructDecl = newExpr.StructDecl())
        EmitStructCtor(*pStructDecl, newExpr.CtorFunction(), newExpr.Args(),
                       newExpr.ClassName(), m_resultOffset);
    else
        EmitNewObject(newExpr.ClassDecl(), newExpr.CtorFunction(),
                      newExpr.Args(), newExpr.ClassName(), m_resultOffset);
}

//Shared new-object emission: `new Foo(args)` and the 0.9.0 ctor-invoke
//twin (`Foo(args)` without new) land here. Both failure modes are
//internal invariant breaks (Round-12) — codegen only runs when the
//front-end saw no errors.
void VmBackend::EmitNewObject(SnClassDecl* pClassDecl,
                              SnFunction* pCtorFn,
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
    const uint16_t classIdx =
        static_cast<uint16_t>(ClassSlotFor(*pClassDecl));
    //0.9.11 T6 (D10): the resolver-selected ctor drives the call — the
    //first-same-name scan is gone. A registered selection calls through
    //its function slot; a null selection (builtin/generic intrinsic ctor,
    //or a type without ctors) keeps the table path; a selected-but-
    //unregistered ctor mirrors EmitStructCtor: plain alloc, arguments
    //unevaluated, no call.
    SnFunction* pRegistered = RegisteredCtor(pCtorFn);
    if (pRegistered) {
        const uint16_t ctorIdx =
            static_cast<uint16_t>(FunctionSlotFor(*pRegistered));
        EmitCtorInvocation(pRegistered, ctorArgs, pSkipArg, *m_pCurrEmitter,
                           ctorIdx, CtorAlloc{false, classIdx, 0},
                           resultOffset);
        return;
    }
    if (!pCtorFn) {
        //Own/builtin records carry the ctor index in the table; a cross-
        //unit placeholder without a selection falls back to the AST scan
        //(defensive: the resolver selects for every user-type call).
        const uint16_t ctorIdx = CtorSlotFor(*pClassDecl, classIdx);
        if (ctorIdx != 0xFFFF) {
            EmitCtorInvocation(nullptr, ctorArgs, pSkipArg, *m_pCurrEmitter,
                               ctorIdx, CtorAlloc{false, classIdx, 0},
                               resultOffset);
            return;
        }
    }

    BytecodeEmitter& emitter = *m_pCurrEmitter;
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(classIdx);
}

//The type has a registered ctor — claim {this, formals...} in evalArea,
//emit one FormalBinding per formal (caller args first, then the defaults
//of the omitted trailing formals — P3), allocate the object into
//resultOffset (OP_New for a class, OP_AllocStruct for a struct — see
//CtorAlloc), bulk-copy the claim to callParamBase, then
//OP_CallMethodDirect on the ctor. pSkipArg is the class-name child the
//NewExpr arg view carries (skipped by identity; null for the invoke
//twin). The resolver rejects named/out args and range-checks the count,
//so every supplied entry is a positional value expression. pCtorFn is
//the AST ctor for the default-binding pass; null for builtins/generics
//and for shapes the front end never binds one for.
void VmBackend::EmitCtorInvocation(SnFunction* pCtorFn,
                                   SnExpressionList& ctorArgs,
                                   SnExpression* pSkipArg,
                                   BytecodeEmitter& emitter, uint16_t ctorIdx,
                                   const CtorAlloc& alloc,
                                   uint16_t resultOffset) {
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
    EmitCtorCallTail(resultOffset, claimBase, n, alloc, ctorIdx, emitter);
    //claim releases on scope exit.
}

//0.9.11 D2/D3: struct construction lowering (see VmBackend.h). The
//allocator is OP_AllocStruct; the claim layout, this staging, and the
//dispatch tail are the class path's (EmitCtorInvocation/EmitCtorCallTail).
//An unregistered selected ctor (body-less, non-native) mirrors the class
//CtorSlotFor 0xFFFF shape: allocate only — the arguments are not
//evaluated and no call is emitted (D3: the call forms still compile).
void VmBackend::EmitStructCtor(SnStructDecl& structDecl,
                               SnFunction* pCtorFn,
                               SnExpressionList& ctorArgs,
                               SnExpression* pSkipArg,
                               uint16_t resultOffset) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    const uint16_t structIdx =
        static_cast<uint16_t>(StructSlotFor(structDecl));
    const uint16_t fieldCount =
        static_cast<uint16_t>(structDecl.FieldCount());
    SnFunction* pRegistered = RegisteredCtor(pCtorFn);
    if (!pRegistered) {
        emitter.Emit(OpCode::OP_AllocStruct);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(structIdx);
        emitter.EmitUint16(fieldCount);
        return;
    }
    const uint16_t ctorIdx =
        static_cast<uint16_t>(FunctionSlotFor(*pRegistered));
    EmitCtorInvocation(pRegistered, ctorArgs, pSkipArg, emitter, ctorIdx,
                       CtorAlloc{true, structIdx, fieldCount},
                       resultOffset);
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
    //K2 slice 5: stays Dual — the size expression's nested emissions
    //park their intermediates in this slot (same resultOffset flow as
    //the binary right claim; a plain int32 only when the size expr is
    //trivially typed, which is not provable here).
    EvalAreaClaim sizeClaim(*this, 1);
    uint16_t sizeSlot = sizeClaim.base();
    EmitExpression(sizeExpr, emitter, sizeSlot);
    emitter.Emit(OpCode::OP_AllocArrayN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(1);  // rank: the single-size form is rank 1
    emitter.EmitUint16(sizeSlot);  // dimsBase (one dimension)
}
} //namespace nlang
