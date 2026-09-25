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

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes
void VmBackend::Access(SnNewExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto& newExpr = static_cast<SnNewExpr&>(expr);
    int classIdx = ResolveNewExprClassIdx(newExpr);
    uint16_t ctorIdx = m_compiledModule.classes[classIdx].constructorIdx;
    uint16_t allocSlot = resultOffset;
    if (ctorIdx != 0xFFFF) {
        EmitCtorInvocation(newExpr, emitter,
                           static_cast<uint16_t>(classIdx), ctorIdx,
                           resultOffset);
        return;
    }

    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(allocSlot);
    emitter.EmitUint16(static_cast<uint16_t>(classIdx));
    return;
}

//Resolve a new-expression's target class to its module index. Both failure
//modes below are internal invariant breaks (Round-12).
int VmBackend::ResolveNewExprClassIdx(SnNewExpr& newExpr) {
    auto* pClassDecl = newExpr.ClassDecl();
    if (!pClassDecl) {
        //Round-12: the resolver failed to bind a class declaration to
        //this new-expression. Codegen only runs when the front-end saw
        //no errors, so this is an internal invariant break.
        throw std::runtime_error(
            "NLang backend: new expression without a bound class declaration");
    }
    //Phase 8e-3: generic instantiations (List<int>) share one CompiledClass
    //named "List" at runtime (erasure). BaseName() returns the unqualified
    //"List" for generic instances, or the full name for ordinary classes.
    const std::string& className = pClassDecl->BaseName();
    int classIdx = m_compiledModule.FindClass(className);
    if (classIdx < 0) {
        //Round-12: the class was resolved by the front-end but never
        //registered in the compiled module. This means the class has
        //no compiled representation — fail the build instead of
        //silently emitting zero (which would make `new Foo()` return
        //null at runtime, a subtle wrong-code bug).
        throw std::runtime_error(
            "NLang backend: new expression for unregistered class: "
            + className);
    }
    return classIdx;
}

//Count a new-expression's positional ctor args. Args() is a view over ALL
//children and its LAST element is the AddChild'ed class-name node (SnNewExpr
//ctor appends it after the ctor args) — skip it by identity. The resolver
//rejects named/out args and validates arity, so every remaining entry is a
//positional value expression.
int VmBackend::CountNewExprCtorArgs(SnNewExpr& newExpr) {
    size_t argCount = 0;
    for (auto& param : newExpr.Args()) {
        if (&param == newExpr.ClassName()) continue;
        ++argCount;
    }
    return static_cast<int>(argCount);
}

//NewExpr arm: the class has a ctor — claim {this, args...} in evalArea,
//emit the args there, OP_New into resultOffset, bulk-copy the claim to
//callParamBase, then OP_CallMethodDirect on the ctor.
void VmBackend::EmitCtorInvocation(SnNewExpr& newExpr, BytecodeEmitter& emitter,
                                   uint16_t classIdx, uint16_t ctorIdx,
                                   uint16_t resultOffset) {
    uint16_t n = static_cast<uint16_t>(1 + CountNewExprCtorArgs(newExpr));  //this + args

    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();

    //Emit args to claim[1..N]. Nested calls in args claim deeper
    //slices — they bulk-copy to callParamBase but that's fine;
    //our this/args live in evalArea, not callParamBase.
    uint16_t paramIdx = 1;
    for (auto& param : newExpr.Args()) {
        if (&param == newExpr.ClassName()) continue;
        uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
        EmitExpression(param, emitter, paramOffset);
        ++paramIdx;
    }
    //Allocate to resultOffset (no conflict with args — they're in evalArea).
    uint16_t allocSlot = resultOffset;

    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(allocSlot);
    emitter.EmitUint16(classIdx);

    //Copy this (new object heap index) to claim[0].
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(allocSlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claimBase);

    //Bulk-copy evalArea claim → callParamBase just before the call.
    for (uint16_t i = 0; i < n; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * VALUE_SIZE);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
    }
    emitter.Emit(OpCode::OP_CallMethodDirect);
    emitter.EmitUint16(ctorIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    emitter.Emit(OpCode::OP_ParaEnd);
    //claim releases on scope exit.
}

    // This expression - reads the implicit first parameter
void VmBackend::Access(SnNewArrayExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto& newArr = static_cast<SnNewArrayExpr&>(expr);
        //Register the array type (idempotent)
        uint16_t arrayTypeIdx = RegisterArrayType(
            newArr.ElementType()->Field());
        //Size staging: EvalAreaClaim, never a temp. A temp staging slot
        //lets a nested binary (`1 + use(make(1))`) park its LEFT operand
        //there while a struct-by-value argument deep-copies through
        //tempSlot (EmitBinding CopyStruct scratch) — silently corrupting
        //the parked value. Claim slots are exclusive to this expression.
        //ExprPeakDepth's NewArrayExpr case tracks the claim=1.
        EvalAreaClaim sizeClaim(*this, 1);
        uint16_t sizeSlot = sizeClaim.base();
        EmitExpression(*newArr.Size(), emitter, sizeSlot);
        emitter.Emit(OpCode::OP_AllocArray);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(arrayTypeIdx);
        emitter.EmitUint16(sizeSlot);
        return;
}
} //namespace nlang
