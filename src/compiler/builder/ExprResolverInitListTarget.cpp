/*---
    ExprResolverInitListTarget.cpp — 集合初始化器目标判定
    从 ExprResolverInitList.cpp 抽取（2026-10-22 source-size 拆分，零行为变化：
    原文件 545 行超 500 行上限、ResolveInitListTarget 51 行超 50 行上限）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
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
    {
        //Stage-2 ND: a rank>=2 token stays the target - the
        //rectangularity gate and the row propagation need the rank;
        //only the rank-1 token peels to its element.
        auto *pToken = static_cast<SnArrayTypeToken*>(pTargetField);
        if (pToken->Rank() < 2)
            pTargetField = pToken->ElemTypeOf();
    }
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

//Stage-2 ND unset-width sentinel (0 is a legal row width: [[], []]).
static const size_t kUnsetWidth = static_cast<size_t>(-1);

//One nesting level of the rectangular walk. expectedWidth carries the
//sibling-agreed entry count at this level (kUnsetWidth = unset; the
//first row sets it): a rectangle means every level partitions into
//equal-width rows, and rows above the leaf level are themselves init
//lists. True = rejected (the walk logged why).
//Stage-3 (D3): above the leaf level a node holding any expression
//entry defers its shape to the runtime builder — no width gate and no
//descent for that node (its literal rows still validate on their own
//resolution). Leaf-level rows (level == rank) are all-scalar and keep
//the width gate, so all-literal trees reject exactly as in stage 2.
bool ExprResolveAccessor::RejectNDShapeLevel(SnInitListExpr &sn,
    uint8_t rank, uint8_t level, size_t &expectedWidth)
{
    const auto &entries = sn.Entries();
    if (level < rank)
    {
        for (auto &entry : entries)
        {
            //A brace list stays a shape error (arrays take the bracket
            //form); only a non-list EXPRESSION defers to the runtime.
            auto *pValue = entry.pValue;
            if (!SnInitListExpr::IsBracketForm(pValue)
                && !(pValue && UntypedInitListOf(*pValue)))
            {
                return false;
            }
        }
    }
    if (expectedWidth == kUnsetWidth)
        expectedWidth = entries.size();
    else if (entries.size() != expectedWidth)
    {
        m_Env.Log(CLL_Error, sn.Location(),
            "initializer rows must have equal width");
        return true;
    }
    if (level >= rank)
        return false;
    size_t childWidth = kUnsetWidth;
    for (auto &entry : entries)
    {
        auto *pRow = SnInitListExpr::IsBracketForm(entry.pValue)
            ? static_cast<SnInitListExpr*>(entry.pValue) : nullptr;
        if (!pRow)
        {
            m_Env.Log(CLL_Error,
                entry.pValue ? entry.pValue->Location() : sn.Location(),
                "initializer depth does not match the declared "
                "array rank");
            return true;
        }
        if (RejectNDShapeLevel(*pRow, rank, level + 1, childWidth))
            return true;
    }
    return false;
}

//Stage-2 ND: the rank>=2 array initializer's shape gate - the top
//level must be non-empty (no inner row exists to infer dims[1] from;
//the rank-1 `int[] a = [];` empty form keeps its legal status) and the
//literal must be rectangular at compile time. True = rejected.
bool ExprResolveAccessor::RejectBadNDInitListShape(SnInitListExpr &sn,
    SnArrayTypeToken &token)
{
    if (sn.Entries().empty())
    {
        m_Env.Log(CLL_Error, sn.Location(),
            "empty initializer gives no row to infer the dimensions "
            "of a multi-dimensional array");
        return true;
    }
    size_t topWidth = kUnsetWidth;
    return RejectNDShapeLevel(sn, token.Rank(), 1, topWidth);
}

} //namespace nlang
