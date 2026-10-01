/*---
    Types.cpp — 类型分派与默认值提取工具
    （RuntimeTypeKind / ExtractDefaultValue / BoxingTagFor / SerializedReturnKind）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/RnTypes.h>

namespace nlang {

//Enum types are int32 at runtime. Struct types use RTK_Struct.
//Array-ness is read through the IsArrayType() polymorphic hook: both a
//syntactic SnArrayTypeExpr and an interned SnArrayTypeToken (0.7.3 B —
//what resolved declarations and array-valued expressions bind to)
//return true, so one check covers type shapes and value tokens alike.
uint8_t VmBackend::RuntimeTypeKind(SnField* pType) {
    if (!pType) return RTK_Int32;
    if (pType->IsArrayType()) return RTK_Array;
    auto k = pType->Kind();
    if (k == NK_EnumDecl) return RTK_Int32;
    if (k == NK_StructDecl) return RTK_Struct;
    if (k == NK_ClassDecl) {
        //Phase 13: synthetic Func<...> declarations are function-handle
        //values (RTK_Func), not classes. Every kind-keyed consumer (local
        //descriptors, field kinds, GC gates) dispatches on this, so the
        //fallthrough to RTK_Class must not swallow them.
        if (static_cast<SnClassDecl*>(pType)->IsFuncType())
            return RTK_Func;
        return RTK_Class;
    }
    return static_cast<uint8_t>(k);
}

//Option B Step 3: extract a constant-foldable default expression into a
//DefaultValueDesc suitable for serialization. Accepts:
//  - null literal (NF_NullLiteral SnLiteralExpr) → RTK_Null, no payload
//  - SnLiteralExpr with NK_Int32 kind                 → RTK_Int32
//  - SnLiteralExpr with NK_Float kind                 → RTK_Float
//  - SnLiteralExpr with NK_String kind                → RTK_String (pool idx)
//  - SnBinaryExpr(OP_Neg, SnLiteralExpr NK_Int32)     → RTK_Int32 (negative)
//Anything else (identifier ref, function call, cast, member access, etc.)
//returns RTK_Void — caller-side (Step 5 declaration check) rejects this
//for IsImported functions. In-module callers don't consult this vector
//at all, so a RTK_Void entry is harmless for them.
//Direct-literal arm of ExtractDefaultValue. Fills dv from a null / int /
//float / string literal; an unknown literal type leaves dv at its
//RTK_Void default (not foldable).
void VmBackend::ExtractLiteralDefault(SnLiteralExpr* lit, DefaultValueDesc& dv) {
    //Null literal: stamped with NF_NullLiteral by KT_Null rule.
    if (lit->ContainFlags(NF_NullLiteral)) {
        dv.tag = RTK_Null;
        return;
    }
    //Type-driven literal dispatch. SnLiteralExpr's Variant Type()
    //points at the RnDataType — match pointer identity against the
    //global singletons (RnInt32/RnFloat/RnString).
    auto* litType = lit->Value().Type();
    if (litType == RnInt32::Instance()) {
        dv.tag = RTK_Int32;
        dv.intValue = static_cast<uint32_t>(
            lit->Value().Data().m_Int);
        return;
    }
    if (litType == RnFloat::Instance()) {
        dv.tag = RTK_Float;
        dv.floatValue = lit->Value().Data().m_Float;
        return;
    }
    if (litType == RnString::Instance()) {
        dv.tag = RTK_String;
        //Intern into producer's pool. Consumer remaps during load.
        auto* sPtr = lit->Value().Data().m_String;
        dv.stringIdx = AddStringConstant(sPtr ? *sPtr : std::string());
        return;
    }
}

//Unary-negation arm of ExtractDefaultValue: fold OP_Neg over an int or
//float literal into dv. Any other shape leaves dv at its RTK_Void
//default (other binary exprs are not supported in MVP).
void VmBackend::ExtractNegatedLiteralDefault(SnBinaryExpr* bin,
                                             DefaultValueDesc& dv) {
    if (bin->Op() == SnBinaryExpr::OP_Neg
        && bin->Left() && bin->Left()->Kind() == NK_LiteralExpr) {
        auto* lit = static_cast<SnLiteralExpr*>(bin->Left());
        if (lit->Value().Type() == RnInt32::Instance()) {
            dv.tag = RTK_Int32;
            int32_t neg = -lit->Value().Data().m_Int;
            dv.intValue = static_cast<uint32_t>(neg);
            return;
        }
        if (lit->Value().Type() == RnFloat::Instance()) {
            dv.tag = RTK_Float;
            dv.floatValue = -lit->Value().Data().m_Float;
            return;
        }
    }
}

DefaultValueDesc VmBackend::ExtractDefaultValue(SnExpression* pExpr) {
    DefaultValueDesc dv;  // tag defaults to RTK_Void
    if (!pExpr) return dv;  // no default expression

    //Direct literal.
    if (pExpr->Kind() == NK_LiteralExpr) {
        ExtractLiteralDefault(static_cast<SnLiteralExpr*>(pExpr), dv);
        return dv;
    }

    //Unary negation of numeric literal: `-5` / `-3.14` parse as OP_Neg
    //over a literal. Fold both int and float so negative floats work
    //cross-module too (not just negative ints).
    if (pExpr->Kind() == NK_BinaryExpr) {
        ExtractNegatedLiteralDefault(static_cast<SnBinaryExpr*>(pExpr), dv);
        return dv;
    }

    return dv;  //non-literal, non-foldable
}

//Phase 8e-4: returns the RTK_* boxing tag for a generic type argument,
//plus an isPrimitive flag. The flag is needed because RTK_Int32 == 0, so
//"is class-T (no boxing)" and "is int-T (box as RTK_Int32)" both yield tag=0.
//Equivalent to the bool needsBoxing + uint8_t tTag pair from the Phase 8e-3
//C1 fix; refactored here so List and Dict can share the helper.
VmBackend::BoxingTagResult VmBackend::BoxingTagFor(SnField* pT) {
    if (!pT) return {0, false};
    NodeKind k = pT->Kind();
    if (k == NK_Int32 || k == NK_EnumDecl) return {RTK_Int32, true};
    if (k == NK_Float)  return {RTK_Float,  true};
    if (k == NK_String) return {RTK_String, true};
    return {0, false};  //class/struct/other T → no boxing
}

//Return-type kind for .ncu serialization (caller checks HasReturn()).
//0.7.3 B: an array return type binds Field() to its interned array
//token, so RuntimeTypeKind alone yields RTK_Array. The pre-token
//IsArrayType() short-circuit existed because Field() degraded `int[]`
//to its element and imported array-returning stubs masqueraded as int
//at type-check sites. Null (unresolved) keeps the legacy 0 sentinel.
uint16_t VmBackend::SerializedReturnKind(SnFunction& func)
{
    SnField* pRetField = func.ReturnType()->Field();
    return pRetField ? static_cast<uint16_t>(RuntimeTypeKind(pRetField)) : 0;
}

} //namespace nlang
