/*--- TypeArgInference.cpp — 调用点类型实参推断的统一走查（契约见
    TypeArgInference.h）。形参侧走类型语法树（占位符叶子在任意深度都
    绑定），实参侧走已解析类型字段的节点结构（数组记号、嵌套实例化），
    两侧按位置配对。Task 5补齐：返回位臂（期望类型经赋值目标发布，
    返回类型树对期望类型再合一）、形参间占位符冲突捕获、返回位独占
    未绑定标记。
---*/
#include <nlang/compiler/TypeArgInference.h>
#include "SnMisc.h"
#include "SnData.h"
#include "SnArrayTypeToken.h"
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnExpressions.h>
#include <vector>

namespace nlang
{

namespace
{

//Leaf formal (name or pre-bound builtin identifier): a placeholder
//binds the whole actual; a concrete leaf requires identity (interned
//singletons make same-type comparisons hold; no widening in inference
//— a mismatch rejects the candidate, and the ordinary binding path
//reports the precise per-argument note). Two formals binding the same
//placeholder to different actuals is the named-conflict shape — the
//first such disagreement is captured for the diagnostic when the
//caller passed a conflict sink.
bool UnifyFormalLeaf(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings,
    TypeArgConflict* pOutConflict)
{
    const SnField* pLeaf = formal.Field();
    if (!pLeaf)
        return false;
    if (!generics.IsPlaceholder(pLeaf))
        return pLeaf == &actual;
    const size_t ordinal = generics.PlaceholderIndex(pLeaf);
    if (ordinal >= bindings.size())
        return false;
    if (bindings[ordinal] && bindings[ordinal] != &actual)
    {
        if (pOutConflict && !pOutConflict->pFirst)
        {
            pOutConflict->ordinal = ordinal;
            pOutConflict->pFirst = bindings[ordinal];
            pOutConflict->pSecond = &actual;
        }
        return false;
    }
    bindings[ordinal] = &actual;
    return true;
}

bool UnifyFormalExpr(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings,
    TypeArgConflict* pOutConflict);

//Nested instantiation formal (List<T>) against an actual instantiation
//of the same template (the erasure identity is the bare name); recurses
//per argument.
bool UnifyFormalInstantiation(const GenericInstanceRegistry& generics,
    const SnGenericTypeExpr& genFormal, const SnClassDecl& instActual,
    std::vector<const SnField*>& bindings,
    TypeArgConflict* pOutConflict)
{
    //The formal's own resolved field is the instantiation node the
    //signature pass minted over its argument spellings — same bare
    //name or the shapes cannot pair.
    const SnField* pFormalInst = genFormal.Field();
    if (!pFormalInst || pFormalInst->Kind() != NK_ClassDecl
        || static_cast<const SnClassDecl*>(pFormalInst)->BaseName()
            != instActual.BaseName())
        return false;
    auto formalArgs = genFormal.TypeArgs();
    const auto& actualArgs = instActual.GenericTypeArgs();
    if (formalArgs.size() != actualArgs.size())
        return false;
    for (size_t i = 0; i < formalArgs.size(); ++i)
        if (!UnifyFormalExpr(generics, *formalArgs[i],
                *actualArgs[i], bindings, pOutConflict))
            return false;
    return true;
}

//One formal tree node against one actual resolved type field. Bindings
//is indexed by placeholder ordinal (grown to the template arity by the
//caller); a second binding of the same ordinal must repeat the field.
bool UnifyFormalExpr(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings,
    TypeArgConflict* pOutConflict)
{
    const auto kind = formal.Kind();
    if (kind == NK_NameExpr || kind == NK_IdentifierExpr)
        return UnifyFormalLeaf(generics, formal, actual, bindings,
            pOutConflict);
    //Array formal (T[], int[,]): the actual must be an array token of
    //the same rank; recurse into the element.
    if (kind == NK_ArrayTypeExpr)
    {
        auto& arrFormal = static_cast<const SnArrayTypeExpr&>(formal);
        if (actual.Kind() != NK_ArrayTypeToken)
            return false;
        auto& token = static_cast<const SnArrayTypeToken&>(actual);
        if (token.Rank() != arrFormal.Rank())
            return false;
        return UnifyFormalExpr(generics, *arrFormal.ElementType(),
            *token.ElemTypeOf(), bindings, pOutConflict);
    }
    if (kind == NK_GenericTypeExpr && actual.Kind() == NK_ClassDecl
        && static_cast<const SnClassDecl&>(actual).IsGenericInstantiation())
        return UnifyFormalInstantiation(generics,
            static_cast<const SnGenericTypeExpr&>(formal),
            static_cast<const SnClassDecl&>(actual), bindings,
            pOutConflict);
    return false;
}

//The placeholder ordinals a resolved type-expression tree mentions —
//the leaf test recursed through array and instantiation shapes. Drives
//the return-position-only unbound hint.
void CollectMentionedOrdinals(const GenericInstanceRegistry& generics,
    const SnFieldExpr& typeExpr, std::vector<size_t>& outOrdinals)
{
    const auto kind = typeExpr.Kind();
    if (kind == NK_NameExpr || kind == NK_IdentifierExpr)
    {
        const SnField* pLeaf = typeExpr.Field();
        if (pLeaf && generics.IsPlaceholder(pLeaf))
            outOrdinals.push_back(generics.PlaceholderIndex(pLeaf));
        return;
    }
    if (kind == NK_ArrayTypeExpr)
        return CollectMentionedOrdinals(generics,
            *static_cast<const SnArrayTypeExpr&>(typeExpr).ElementType(),
            outOrdinals);
    if (kind == NK_GenericTypeExpr)
        for (const SnFieldExpr* pArg
            : static_cast<const SnGenericTypeExpr&>(typeExpr).TypeArgs())
            CollectMentionedOrdinals(generics, *pArg, outOrdinals);
}

//The template's type-parameter count: a free function names its own
//parameters on the declaration (0.13.1a Task 4); a template-row member
//inherits the owning class's (Task 2's shape). Zero = nothing to infer.
size_t TemplateTypeParamArity(const SnFunction& tmplMember)
{
    if (tmplMember.IsGenericDeclaration())
        return tmplMember.TypeParamNames().size();
    const SyntaxNode* pOwner = tmplMember.Parent();
    return pOwner && pOwner->Kind() == NK_ClassDecl
        ? static_cast<const SnClassDecl*>(pOwner)
            ->TypeParamNames().size()
        : 0;
}

//The formal-binding loop: each positional actual unifies its formal's
//type tree. False = mismatch; rConflict carries both sides when the
//unify captured them.
bool BindFormalsFromActuals(const GenericInstanceRegistry& generics,
    const SnFunction& tmplMember,
    const std::vector<const SnExpression*>& positional,
    std::vector<const SnField*>& bindings, TypeArgConflict& rConflict)
{
    auto iParamIt = tmplMember.Params().begin();
    for (const SnExpression* pActual : positional)
    {
        //Defaults cover the tail; no actual past the formals, no
        //binding from it.
        if (iParamIt == tmplMember.Params().end())
            break;
        const SnFieldExpr* pFormal = iParamIt->Type();
        if (pFormal && !UnifyFormalExpr(generics, *pFormal,
                *pActual->EvalDataType(), bindings, &rConflict))
            return false;
        ++iParamIt;
    }
    return true;
}

//Return-position arm: placeholders the formals left unbound unify
//the return-type tree against the expected type (the assign-published
//target). The scratch copy keeps a failed return unify from corrupting
//formal bindings — the ordinary assignment type check reports a
//genuine target mismatch afterwards.
void UnifyReturnPosition(const GenericInstanceRegistry& generics,
    const SnFunction& tmplMember, const SnField* pExpectedType,
    std::vector<const SnField*>& bindings)
{
    if (!pExpectedType)
        return;
    if (auto* pReturnType = tmplMember.ReturnType())
    {
        auto returnBindings = bindings;
        if (UnifyFormalExpr(generics, *pReturnType, *pExpectedType,
                returnBindings, nullptr))
            bindings = std::move(returnBindings);
    }
}

//Marks the first unbound placeholder in the outcome; the return-only
//hint fires when the return-type tree mentions it (any formal or
//expected-type source would have bound it).
void MarkFirstUnbound(const GenericInstanceRegistry& generics,
    const SnFunction& tmplMember,
    const std::vector<const SnField*>& bindings,
    TypeArgInferenceOutcome& outcome)
{
    for (size_t ordinal = 0; ordinal < bindings.size(); ++ordinal)
    {
        if (bindings[ordinal])
            continue;
        outcome.result = TypeArgInferenceResult::Unbound;
        if (auto* pReturnType = tmplMember.ReturnType())
        {
            std::vector<size_t> mentioned;
            CollectMentionedOrdinals(generics, *pReturnType, mentioned);
            for (size_t m : mentioned)
                if (m == ordinal)
                {
                    outcome.bReturnOnlyUnbound = true;
                    break;
                }
        }
        break;   //the diagnostic names the first unbound parameter
    }
}

} //namespace

TypeArgInferenceOutcome InferTypeArgsForCall(
    const GenericInstanceRegistry& generics, const SnFunction& tmplMember,
    const std::vector<SnExpression*>& actuals,
    const SnField* pExpectedType)
{
    TypeArgInferenceOutcome outcome;
    //Every actual must be a resolved value with a type; anything else
    //(a named argument's inner, an unresolved literal) leaves the call
    //to the ordinary binding path.
    std::vector<const SnExpression*> positional;
    positional.reserve(actuals.size());
    for (const SnExpression* pActual : actuals)
    {
        if (!pActual || !pActual->IsResolved() || !pActual->EvalDataType())
            return outcome;   //Mismatched, no detail
        positional.push_back(pActual);
    }

    const size_t arity = TemplateTypeParamArity(tmplMember);
    if (arity == 0)
        return outcome;       //Mismatched, no detail
    std::vector<const SnField*> bindings(arity, nullptr);

    TypeArgConflict conflict;
    if (!BindFormalsFromActuals(generics, tmplMember, positional,
            bindings, conflict))
    {
        outcome.conflict = conflict;
        return outcome;       //Mismatched; conflict detail when captured
    }
    UnifyReturnPosition(generics, tmplMember, pExpectedType, bindings);
    MarkFirstUnbound(generics, tmplMember, bindings, outcome);
    if (outcome.result == TypeArgInferenceResult::Unbound)
        return outcome;
    //Bound: hand the registry-facing fields to the caller (the unify
    //internals stay const; this is the single widening point).
    outcome.bindings.reserve(arity);
    for (const SnField* pBound : bindings)
        outcome.bindings.push_back(const_cast<SnField*>(pBound));
    outcome.result = TypeArgInferenceResult::Bound;
    return outcome;
}

} //namespace nlang
