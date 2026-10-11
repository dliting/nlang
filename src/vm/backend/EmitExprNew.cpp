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
        EmitCtorInvocation(ctorArgs, pSkipArg, *m_pCurrEmitter, classIdx,
                           ctorIdx, resultOffset);
        return;
    }

    BytecodeEmitter& emitter = *m_pCurrEmitter;
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(classIdx);
}

//The class has a ctor — claim {this, args...} in evalArea, emit the args
//there, OP_New into resultOffset, bulk-copy the claim to callParamBase,
//then OP_CallMethodDirect on the ctor. pSkipArg is the class-name child
//the NewExpr arg view carries (skipped by identity; null for the invoke
//twin). The resolver rejects named/out args and validates arity, so every
//remaining entry is a positional value expression.
void VmBackend::EmitCtorInvocation(SnExpressionList& ctorArgs,
                                   SnExpression* pSkipArg,
                                   BytecodeEmitter& emitter, uint16_t classIdx,
                                   uint16_t ctorIdx, uint16_t resultOffset) {
    size_t argCount = 0;
    for (auto& param : ctorArgs) {
        if (&param == pSkipArg) continue;
        ++argCount;
    }
    const uint16_t n = static_cast<uint16_t>(1 + argCount);  //this + args

    EvalAreaClaim claim(*this, n);
    uint16_t claimBase = claim.base();

    //Emit args to claim[1..N]. Nested calls in args claim deeper
    //slices — they bulk-copy to callParamBase but that's fine;
    //our this/args live in evalArea, not callParamBase.
    uint16_t paramIdx = 1;
    for (auto& param : ctorArgs) {
        if (&param == pSkipArg) continue;
        uint16_t paramOffset = claimBase + paramIdx * kFrameSlotBytes;
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
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->callParamBase + i * kFrameSlotBytes);
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
