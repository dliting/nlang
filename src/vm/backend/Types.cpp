/*---
    Types.cpp — 类型分派与默认值提取工具
    （RuntimeTypeKind / ExtractDefaultValue / BoxingTagFor / SerializedReturnKind）。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "EmitPrimOps.h"
#include <nlang/vm/ExprDesc.h>
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
        //Phase 13: synthetic func<...> declarations are function-handle
        //values (RTK_Func), not classes. Every kind-keyed consumer (local
        //descriptors, field kinds, GC gates) dispatches on this, so the
        //fallthrough to RTK_Class must not swallow them.
        if (static_cast<SnClassDecl*>(pType)->IsFuncType())
            return RTK_Func;
        return RTK_Class;
    }
    //Builtin data types: NK_* and RTK_* are independent numberings since
    //the basic-types expansion (NK_String=12 vs RTK_String=2 — the old
    //layout held a coincidental identity for int/float/string). Scalars
    //map through the registry; string is the one non-scalar builtin.
    //Anything else keeps the null-type defensive default.
    if (k == NK_String) return RTK_String;
    const uint8_t rtk = RtkOfKind(k);
    return rtk != 0xFF ? rtk : RTK_Int32;
}

//Option B Step 3: extract a constant-foldable default expression into a
//DefaultValueDesc suitable for serialization. Accepts:
//  - null literal (NF_NullLiteral SnLiteralExpr) → RTK_Null, no payload
//  - SnLiteralExpr with NK_Int32 kind                 → RTK_Int32
//  - SnLiteralExpr with NK_Float kind                 → RTK_Float
//  - SnLiteralExpr with NK_String kind                → RTK_String (pool idx)
//  - SnBinaryExpr(OP_Neg, SnLiteralExpr NK_Int32)     → RTK_Int32 (negative)
//  - 0.7.5: long/ulong/double literals (and their negations) fold the
//    same way through the 8-byte channels
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
    if (litType == RnDouble::Instance()) {
        dv.tag = RTK_Double;
        dv.doubleValue = lit->Value().Data().m_Double;
        return;
    }
    //0.7.5: 8-byte scalar literals — kind-exact Variant member reads.
    if (litType == RnLong::Instance()) {
        dv.tag = RTK_Long;
        dv.longValue = lit->Value().Data().m_Long;
        return;
    }
    if (litType == RnULong::Instance()) {
        dv.tag = RTK_ULong;
        dv.longValue = static_cast<int64_t>(
            lit->Value().Data().m_ULong);
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
        //0.7.5: negated long defaults fold the same way.
        if (lit->Value().Type() == RnLong::Instance()) {
            dv.tag = RTK_Long;
            dv.longValue = -lit->Value().Data().m_Long;
            return;
        }
        if (lit->Value().Type() == RnFloat::Instance()) {
            dv.tag = RTK_Float;
            dv.floatValue = -lit->Value().Data().m_Float;
            return;
        }
        if (lit->Value().Type() == RnDouble::Instance()) {
            dv.tag = RTK_Double;
            dv.doubleValue = -lit->Value().Data().m_Double;
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

//--- v2.3 ExprDesc encoder (complex cross-module defaults) ----------------
//Little-endian append helpers for the wire grammar in CompiledModule.h.
namespace {
void AppendBytes(std::vector<uint8_t>& out, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    out.insert(out.end(), b, b + n);
}
void AppendU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendU32(std::vector<uint8_t>& out, uint32_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendI64(std::vector<uint8_t>& out, int64_t v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendF32(std::vector<uint8_t>& out, float v) {
    AppendBytes(out, &v, sizeof(v));
}
void AppendF64(std::vector<uint8_t>& out, double v) {
    AppendBytes(out, &v, sizeof(v));
}
}  //namespace

//Literal arm of the ExprDesc encoder. Mirrors the fold arms of
//ExtractLiteralDefault (same kind set); bool/char/byte literals stay
//outside the subset and return false.
bool VmBackend::EncodeLiteralDesc(SnLiteralExpr& lit,
                                  std::vector<uint8_t>& out) {
    if (lit.ContainFlags(NF_NullLiteral)) {
        AppendU8(out, RTK_Null);
        return true;
    }
    auto* litType = lit.Value().Type();
    if (litType == RnInt32::Instance()) {
        AppendU8(out, RTK_Int32);
        AppendU32(out, static_cast<uint32_t>(lit.Value().Data().m_Int));
        return true;
    }
    if (litType == RnFloat::Instance()) {
        AppendU8(out, RTK_Float);
        AppendF32(out, lit.Value().Data().m_Float);
        return true;
    }
    if (litType == RnDouble::Instance()) {
        AppendU8(out, RTK_Double);
        AppendF64(out, lit.Value().Data().m_Double);
        return true;
    }
    if (litType == RnLong::Instance()) {
        AppendU8(out, RTK_Long);
        AppendI64(out, lit.Value().Data().m_Long);
        return true;
    }
    if (litType == RnULong::Instance()) {
        AppendU8(out, RTK_ULong);
        AppendI64(out, static_cast<int64_t>(lit.Value().Data().m_ULong));
        return true;
    }
    if (litType == RnString::Instance()) {
        AppendU8(out, RTK_String);
        auto* sPtr = lit.Value().Data().m_String;
        AppendU32(out, AddStringConstant(sPtr ? *sPtr : std::string()));
        return true;
    }
    return false;
}

//Arithmetic arm of the ExprDesc encoder: unary negation and the five
//arithmetic binaries (see CompiledModule.h → ExprDesc.h for the grammar).
bool VmBackend::EncodeBinaryDesc(SnBinaryExpr& bin,
                                 const std::vector<SnFormalParam*>& formals,
                                 std::vector<uint8_t>& out, bool allowCall,
                                 int depth) {
    //Unary negation (unary ctor leaves Right() null).
    if (bin.Op() == SnBinaryExpr::OP_Neg && !bin.Right()) {
        AppendU8(out, ED_Neg);
        return EncodeExprDesc(*bin.Left(), formals, out, allowCall,
                              depth + 1);
    }
    uint8_t opCode = 0;
    switch (bin.Op()) {
    case SnBinaryExpr::OP_Add: opCode = EDB_Add; break;
    case SnBinaryExpr::OP_Sub: opCode = EDB_Sub; break;
    case SnBinaryExpr::OP_Mul: opCode = EDB_Mul; break;
    case SnBinaryExpr::OP_Div: opCode = EDB_Div; break;
    case SnBinaryExpr::OP_Mod: opCode = EDB_Mod; break;
    default: return false;  //comparisons/logical ops: out of subset
    }
    AppendU8(out, ED_Binary);
    AppendU8(out, opCode);
    if (!EncodeExprDesc(*bin.Left(), formals, out, allowCall, depth + 1))
        return false;
    return EncodeExprDesc(*bin.Right(), formals, out, allowCall,
                          depth + 1);
}

//Call arm of the ExprDesc encoder: a producing-module free function,
//fully applied (D4); args encode with allowCall=false (no nested calls,
//D3). The consumer rebuilds a call qualified by THIS module's path.
bool VmBackend::EncodeCallDesc(SnInvokeExpr& invoke,
                               const std::vector<SnFormalParam*>& formals,
                               std::vector<uint8_t>& out, int depth) {
    auto* pCallee = invoke.Callee();
    if (!pCallee || pCallee->IsImported()) return false;
    //Free functions only — methods/enums ride the method path and
    //their keys are enclosure-qualified.
    auto* pParent = pCallee->Parent();
    if (pParent && (pParent->Kind() == NK_ClassDecl
                    || pParent->Kind() == NK_EnumDecl))
        return false;
    const std::string key = KeyOf(*pCallee);
    const std::string prefix = m_compiledModule.modulePath + ".";
    if (key.size() <= prefix.size()
        || key.compare(0, prefix.size(), prefix) != 0)
        return false;
    size_t argCount = 0;
    for (auto& arg : invoke.Params())
        ++argCount;
    if (argCount != pCallee->Params().size() || argCount > UINT8_MAX)
        return false;
    AppendU8(out, ED_Call);
    AppendU16(out, static_cast<uint16_t>(key.size()));
    AppendBytes(out, key.data(), key.size());
    AppendU32(out, static_cast<uint32_t>(argCount));
    AppendU8(out, static_cast<uint8_t>(argCount));
    for (auto& arg : invoke.Params()) {
        if (!EncodeExprDesc(arg, formals, out, false, depth + 1))
            return false;
    }
    return true;
}

//Recursive ExprDesc encoder (see CompiledModule.h → ExprDesc.h for the
//grammar). Returns false for anything outside the subset — the caller
//discards the partial bytes and keeps the RTK_Unfoldable stamp (old
//reject behavior).
bool VmBackend::EncodeExprDesc(SnExpression& expr,
                               const std::vector<SnFormalParam*>& formals,
                               std::vector<uint8_t>& out, bool allowCall,
                               int depth) {
    if (depth > kMaxExprDescDepth) return false;
    switch (expr.Kind()) {
    case NK_LiteralExpr:
        return EncodeLiteralDesc(static_cast<SnLiteralExpr&>(expr), out);

    case NK_IdentifierExpr: {
        auto* pField = static_cast<SnIdentifierExpr&>(expr).Field();
        if (!pField) return false;
        //Formal references only — a global/const default keeps the old
        //reject. The declaration-time forward-ref check guarantees the
        //bound formal precedes the default's own formal.
        for (size_t i = 0; i < formals.size(); ++i) {
            if (pField == formals[i]) {
                AppendU8(out, ED_FormalRef);
                AppendU8(out, static_cast<uint8_t>(i));
                return true;
            }
        }
        return false;
    }

    case NK_BinaryExpr:
        return EncodeBinaryDesc(static_cast<SnBinaryExpr&>(expr), formals,
                                out, allowCall, depth);

    case NK_InvokeExpr:
        if (!allowCall) return false;  //nested calls: out of subset (D3)
        return EncodeCallDesc(static_cast<SnInvokeExpr&>(expr), formals,
                              out, depth);

    default:
        return false;  //casts, member access, init lists, …: old reject
    }
}

//Top entry: encode func's default expression into an ExprDesc tree.
//True = dv carries RTK_Expr + exprBytes; false = outside the subset and
//dv stays without a default (caller stamps RTK_Unfoldable).
bool VmBackend::EncodeDefaultExpr(SnFunction& func, SnExpression* pExpr,
                                  DefaultValueDesc& dv) {
    if (!pExpr) return false;
    std::vector<SnFormalParam*> formals;
    for (auto& param : func.Params())
        formals.push_back(&param);
    std::vector<uint8_t> bytes;
    if (!EncodeExprDesc(*pExpr, formals, bytes, true, 0)
        || bytes.size() > kMaxExprDescBytes)
        return false;
    dv.tag = RTK_Expr;
    dv.exprBytes = std::move(bytes);
    return true;
}

//Phase 8e-4: returns the RTK_* boxing tag for a generic type argument,
//plus an isPrimitive flag. The flag is needed because RTK_Int32 == 0, so
//"is class-T (no boxing)" and "is int-T (box as RTK_Int32)" both yield tag=0.
//Equivalent to the bool needsBoxing + uint8_t tTag pair from the Phase 8e-3
//C1 fix; refactored here so List and Dict can share the helper.
//0.7.5: registry-driven — every scalar row (and string/enum via their
//carrier tags) boxes; class/struct/array/other kinds keep the no-boxing
//verdict (BoxTypeTagOfKind's 0xFF).
VmBackend::BoxingTagResult VmBackend::BoxingTagFor(SnField* pT) {
    if (!pT) return {0, false};
    uint8_t tag = BoxTypeTagOfKind(pT->Kind());
    if (tag != 0xFF) return {tag, true};
    return {0, false};
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
