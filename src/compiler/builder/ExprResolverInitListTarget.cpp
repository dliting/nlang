/*---
    ExprResolverInitListTarget.cpp — 集合初始化器目标判定
    从 ExprResolverInitList.cpp 抽取（2026-10-22 source-size 拆分，零行为变化：
    原文件 545 行超 500 行上限、ResolveInitListTarget 51 行超 50 行上限）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Shared missing-target diagnostic: the bare-brace fallthrough in
//ResolveInitListTarget and the target-less bracket inference helper
//report the same message.
static const char *const kMsgInitListNoTarget =
    "Collection initializer requires an explicit type or an LHS "
    "context to infer the target type.";

//0.10.8 D2: element type of a bare `[...]` literal with neither an
//explicit type nor an inferred target (method receivers, subscript
//bases and other plain-expression positions). The entries resolve once
//here (the Access entry loop skips the resolved ones); all must share
//one present eval type (pointer identity — the codebase's type-
//comparison idiom). Null = no target; every failing shape logged its
//diagnostic here.
static SnField *InferBracketElemType(SnInitListExpr &sn,
    BuildEnvironment &env, ISyntaxNodeVisitor &visitor)
{
    const auto &entries = sn.Entries();
    if (entries.empty())
    {
        env.Log(CLL_Error, sn.Location(),
            "cannot infer the element type of an empty list literal; "
            "add an explicit type");
        return nullptr;
    }
    SnField *pElem = nullptr;
    for (auto &entry : entries)
    {
        if (entry.pValue && !entry.pValue->IsResolved())
            entry.pValue->Accept(visitor);
        SnField *pType = entry.pValue
            ? entry.pValue->EvalDataType() : nullptr;
        if (!pType)
        {
            //A func reference (no type before binding) or a nested
            //untyped brace: the generic missing-target diagnostic.
            env.Log(CLL_Error, sn.Location(), kMsgInitListNoTarget);
            return nullptr;
        }
        //An array-typed element would mint a token-of-token target —
        //the jagged-array diagnostic owns that shape.
        if (pType->Kind() == NK_ArrayTypeToken)
        {
            env.Log(CLL_Error, sn.Location(),
                "jagged arrays (T[][]) are not supported");
            return nullptr;
        }
        if (pElem && pType != pElem)
        {
            env.Log(CLL_Error, sn.Location(),
                "list literal elements have differing types; "
                "add an explicit type");
            return nullptr;
        }
        pElem = pType;
    }
    return pElem;
}

//Bare form with a parent-supplied target: bIsArray keeps the
//declaration-side signal (IsArrayType()); the type slot carries the
//interned token for array variables, peeled to the element below.
//0.7.3 B token path: an array-typed LHS carries the interned token;
//the init-list's per-entry element gate consumes the ELEMENT (the node
//itself keeps the interned token — Access stores it at the mint point,
//0.9.13 D-new-1).
static void ResolveInferredTarget(SnField *pInferred,
    SnField* &pTargetField, bool &bIsArray)
{
    bIsArray = pInferred->IsArrayType();
    pTargetField = pInferred->EvalDataType();
    if (pTargetField
        && pTargetField->Kind() == NK_ArrayTypeToken)
        pTargetField = static_cast<SnArrayTypeToken*>(
            pTargetField)->ElemTypeOf();
}

//Target determination of the collection initializer (Phase 8e-6):
//- Explicit form `new Type{...}`: ExplicitType() carries the type
//  expression. Resolve it like a normal type, its resolved Field is the
//  target.
//- Bare form `[...]`: ExplicitType() is null. The parent AssignStmt
//  resolver must have populated InferredTarget() from the LHS variable's
//  type.
//False = no target could be derived (diagnostic logged).
bool ExprResolveAccessor::ResolveInitListTarget(SnInitListExpr &sn,
    SnField* &pTargetField, bool &bIsArray)
{
    pTargetField = nullptr;
    bIsArray = false;
    if (auto *pExplicit = sn.ExplicitType())
    {
        //Resolve the explicit type expression (NameExpr/GenericTypeExpr).
        pExplicit->Accept(*m_pVisitor);
        if (!pExplicit->IsResolved())
            return false;
        pTargetField = pExplicit->Field();
        bIsArray = pExplicit->IsArrayType();
    }
    else if (auto *pInferred = sn.InferredTarget())
        ResolveInferredTarget(pInferred, pTargetField, bIsArray);
    else if (sn.IsArrayForm())
    {
        //0.10.8 D2: no target context — infer the element type from
        //the entries (the helper resolved them; the Access entry loop
        //skips the resolved ones). Every failure logged its own
        //diagnostic, so the generic fallthrough message stays brace-only.
        if (auto *pElem = InferBracketElemType(sn, m_Env, *m_pVisitor))
        {
            bIsArray = true;
            pTargetField = pElem;
            return true;
        }
        return false;
    }
    if (pTargetField)
        return true;
    m_Env.Log(CLL_Error, sn.Location(), kMsgInitListNoTarget);
    return false;
}

} //namespace nlang
