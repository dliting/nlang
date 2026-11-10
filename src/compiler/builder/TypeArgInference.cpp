/*--- TypeArgInference.cpp — 调用点类型实参推断的统一走查（契约见
    TypeArgInference.h）。形参侧走类型语法树（占位符叶子在任意深度都
    绑定），实参侧走已解析类型字段的节点结构（数组记号、嵌套实例化），
    两侧按位置配对。
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
//reports the precise per-argument note).
bool UnifyFormalLeaf(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings)
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
        return false;
    bindings[ordinal] = &actual;
    return true;
}

bool UnifyFormalExpr(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings);

//Nested instantiation formal (List<T>) against an actual instantiation
//of the same template (the erasure identity is the bare name); recurses
//per argument.
bool UnifyFormalInstantiation(const GenericInstanceRegistry& generics,
    const SnGenericTypeExpr& genFormal, const SnClassDecl& instActual,
    std::vector<const SnField*>& bindings)
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
                *actualArgs[i], bindings))
            return false;
    return true;
}

//One formal tree node against one actual resolved type field. Bindings
//is indexed by placeholder ordinal (grown to the template arity by the
//caller); a second binding of the same ordinal must repeat the field.
bool UnifyFormalExpr(const GenericInstanceRegistry& generics,
    const SnFieldExpr& formal, const SnField& actual,
    std::vector<const SnField*>& bindings)
{
    const auto kind = formal.Kind();
    if (kind == NK_NameExpr || kind == NK_IdentifierExpr)
        return UnifyFormalLeaf(generics, formal, actual, bindings);
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
            *token.ElemTypeOf(), bindings);
    }
    if (kind == NK_GenericTypeExpr && actual.Kind() == NK_ClassDecl
        && static_cast<const SnClassDecl&>(actual).IsGenericInstantiation())
        return UnifyFormalInstantiation(generics,
            static_cast<const SnGenericTypeExpr&>(formal),
            static_cast<const SnClassDecl&>(actual), bindings);
    return false;
}

//True when a resolved type-expression tree mentions a type-parameter
//placeholder anywhere — the leaf test recursed through array and
//instantiation shapes.
bool ExprMentionsPlaceholder(const GenericInstanceRegistry& generics,
    const SnFieldExpr& typeExpr)
{
    const auto kind = typeExpr.Kind();
    if (kind == NK_NameExpr || kind == NK_IdentifierExpr)
        return generics.IsPlaceholder(typeExpr.Field());
    if (kind == NK_ArrayTypeExpr)
        return ExprMentionsPlaceholder(generics,
            *static_cast<const SnArrayTypeExpr&>(typeExpr).ElementType());
    if (kind == NK_GenericTypeExpr)
    {
        for (const SnFieldExpr* pArg
            : static_cast<const SnGenericTypeExpr&>(typeExpr).TypeArgs())
            if (ExprMentionsPlaceholder(generics, *pArg))
                return true;
    }
    return false;
}

} //namespace

TypeArgInferenceResult InferTypeArgsForCall(
    const GenericInstanceRegistry& generics, const SnFunction& tmplMember,
    const std::vector<SnExpression*>& actuals,
    std::vector<SnField*>& outTypeArgs)
{
    outTypeArgs.clear();
    //Every actual must be a resolved value with a type; anything else
    //(a named argument's inner, an unresolved literal) leaves the call
    //to the ordinary binding path.
    std::vector<const SnExpression*> positional;
    positional.reserve(actuals.size());
    for (const SnExpression* pActual : actuals)
    {
        if (!pActual || !pActual->IsResolved() || !pActual->EvalDataType())
            return TypeArgInferenceResult::Mismatched;
        positional.push_back(pActual);
    }

    const size_t arity = tmplMember.Parent()
        && tmplMember.Parent()->Kind() == NK_ClassDecl
        ? static_cast<const SnClassDecl*>(tmplMember.Parent())
            ->TypeParamNames().size()
        : 0;
    if (arity == 0)
        return TypeArgInferenceResult::Mismatched;
    std::vector<const SnField*> bindings(arity, nullptr);

    size_t iParam = 0;
    for (auto& param : tmplMember.Params())
    {
        if (iParam >= positional.size())
            break;   //defaults cover the tail; no actual, no binding
        const SnFieldExpr* pFormal = param.Type();
        if (pFormal && !UnifyFormalExpr(generics, *pFormal,
                *positional[iParam]->EvalDataType(), bindings))
            return TypeArgInferenceResult::Mismatched;
        ++iParam;
    }

    for (size_t ordinal = 0; ordinal < arity; ++ordinal)
        if (!bindings[ordinal])
            return TypeArgInferenceResult::Unbound;

    outTypeArgs.resize(arity, nullptr);
    for (size_t ordinal = 0; ordinal < arity; ++ordinal)
        outTypeArgs[ordinal] = const_cast<SnField*>(bindings[ordinal]);
    return TypeArgInferenceResult::Bound;
}

} //namespace nlang
