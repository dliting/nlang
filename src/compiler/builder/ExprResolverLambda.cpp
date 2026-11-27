/*---
    ExprResolverLambda.cpp — lambda expression resolution (0.13.10).
    The cover grammar delivers every list item that is not a parameter
    shape as a raw expression; the parameter gate here rejects those by
    name (spec §3.5). Task 4 adds the typing through the expected-type
    channel: the parent's declared func type fills the parameter
    descriptors positionally, the node stamps the func instantiation,
    and the two body arms resolve against the expected return type
    (spec §3.2).
---*/
#include "ExprResolver.h"
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>

namespace nlang
{

//True when any list item arrived as a raw expression. The parser's
//arrow-confirmed fold reorganizes bare identifiers and reinterprets
//the `int& x` spelling into a typed parameter; deeper bit-and chains
//and every other expression shape stay raw (Task 2 rulings) and are
//rejected here.
bool InvalidLambdaParameterList(SnLambdaExpr &sn)
{
	for (const auto &param : sn.Params())
		if (ParamIsRawExpression(param))
			return true;
	return false;
}

//0.13.10: the lambda counterpart of UntypedInitListOf (shared entry
//point of target-type inference — deferred call arguments, statement-
//side propagations, nested entries). Untyped = resolved flag off and
//no target set yet.
SnLambdaExpr *UntypedLambdaOf(SnExpression &expr)
{
	if (expr.Kind() != NK_LambdaExpr || expr.IsResolved())
		return nullptr;
	auto &lambda = static_cast<SnLambdaExpr&>(expr);
	if (lambda.InferredTarget())
		return nullptr;
	return &lambda;
}

//0.13.10 Task 4: the positional parameter fill (spec §3.2). A bare
//parameter takes the expected type's positional argument (the &-mark
//rides the func instantiation's ref-flag row); an explicit spelling
//resolves its type token and must match position-for-position — no
//variance (the pointer identity of the interned type fields is the
//FuncRefMatchesDecl discipline). False = reported.
bool ExprResolveAccessor::FillLambdaParams(SnLambdaExpr &sn,
	SnClassDecl &funcDecl)
{
	auto typeArgs = GetGenericTypeArgs(&funcDecl);
	auto &refFlags = GetGenericRefFlags(&funcDecl);
	for (size_t i = 0; i < sn.Params().size(); ++i)
	{
		const auto &param = sn.Params()[i];
		//The ref-flag row holds 0/1 entries (FuncRefMatchesDecl reads
		//`refFlags[i] != 0`), not flag masks.
		const bool bExpectedRef = (i + 1) < refFlags.size()
			&& refFlags[i + 1] != 0;
		SnField *pParamType = nullptr;
		bool bParamRef = bExpectedRef;
		if (param.pType)
		{
			//The explicit spelling resolves like any type expression
			//(the ResolveInitListTarget explicit-type idiom); the &-mark
			//rides the type wrapper (nlang.y FoldBareIdentifierParams).
			param.pType->Accept(*m_pVisitor);
			if (!param.pType->IsResolved())
				return false;
			pParamType = param.pType->Field();
			bParamRef = param.pType->ContainFlags(NF_Ref);
			auto *pExpected = typeArgs[i + 1];
			if (!pParamType || pParamType != pExpected
				|| bParamRef != bExpectedRef)
			{
				m_Env.Log(CLL_Error,
					param.pType->Location(),
					"lambda parameter \"%s\" does not match the expected "
					"type %s.",
					param.name.c_str(),
					pExpected ? pExpected->ToString().c_str() : "?");
				return false;
			}
		}
		else
		{
			pParamType = typeArgs[i + 1];
		}
		auto *pLocal = sn.AddParamLocal(param.name, pParamType,
			param.pType ? *param.pType->Location() : *sn.Location());
		if (bParamRef)
			pLocal->AddFlags(NF_Ref);
	}
	return true;
}

//0.13.10 Task 4: the expression body arm. The lambda is the body's
//scope base for the walk — the param descriptors answer the body's
//name lookup through the FindField override, and a same-named outer
//local is shadowed for the body only (the context is restored after,
//so sibling subtrees keep the caller's scope).
void ExprResolveAccessor::ResolveLambdaExprBody(SnLambdaExpr &sn,
	SnField *pReturnType)
{
	auto *pBody = sn.ExprBody();
	//A lambda-valued body inherits the return type as its expected
	//function type (the same identity the check below reads).
	if (auto *pInner = UntypedLambdaOf(*pBody))
		pInner->InferredTarget(pReturnType);
	SyntaxNode *pSavedContext = m_pContext;
	m_pContext = &sn;
	pBody->Accept(*m_pVisitor);
	m_pContext = pSavedContext;
	if (!pBody->IsResolved() || !pReturnType->IsResolved())
		return;
	auto *pSourceType = pBody->EvalDataType();
	//A body with no resolved type already reported upstream; the cast
	//report would only bury it (the FinishReturnExpr discipline).
	if (!pSourceType)
		return;
	if (pBody->Kind() == NK_LambdaExpr)
	{
		//Function values match by identity — the stamped func
		//instantiation is interned per signature, so a mismatch is a
		//genuinely different signature, not a spelling difference.
		if (pSourceType != pReturnType)
			m_Env.Log(CLL_Error, pBody->Location(),
				"the lambda's expression body type %s does not match "
				"the expected return type %s.",
				pSourceType->ToString().c_str(),
				pReturnType->ToString().c_str());
		return;
	}
	auto castInfo = GetCastInfo(pSourceType, pReturnType);
	auto iExpr = sn.Children().find(sn.m_pExprBody);
	if (FixupExprType(iExpr, castInfo))
		sn.m_pExprBody = &static_cast<SnCastExpr &>(*iExpr);
}

//0.13.10 Task 4: the block body arm — the back-channel walk plus the
//「块体无return即void」 check (D-series ruling: a return-less block
//against a non-void return type is a named error, never a silent
//garbage value).
void ExprResolveAccessor::ResolveLambdaBlockBodyChannel(
	SnLambdaExpr &sn, SnField *pReturnType)
{
	if (!m_pLambdaBodyResolver)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"the lambda's block body cannot be resolved in this "
			"context.");
		return;
	}
	m_pLambdaBodyResolver->ResolveLambdaBlockBody(sn);
	if (pReturnType->Kind() != NK_Void && !sn.HasBlockValueReturn())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"the lambda's block body never returns a value; the "
			"expected return type %s is not void.",
			pReturnType->ToString().c_str());
	}
}

//Target admission of TypeLambda: the expected type must exist, be a
//function type, and its parameter count must match. nullptr = rejected
//(diagnostic logged); the caller flags the node resolved.
static SnClassDecl *AdmitLambdaTarget(BuildEnvironment &env,
	SnLambdaExpr &sn)
{
	//The target is the parent-declared field; the unwrap is the
	//ResolveInferredTarget convention (a type pushed directly answers
	//itself — SnClassDecl::EvalDataType is identity).
	auto *pTargetField = sn.InferredTarget();
	if (!pTargetField)
	{
		env.Log(CLL_Error, sn.Location(),
			"a lambda expression requires an expected function type.");
		return nullptr;
	}
	auto *pFuncType = pTargetField->EvalDataType();
	if (!pFuncType || !IsFuncTypeDecl(pFuncType))
	{
		env.Log(CLL_Error, sn.Location(),
			"the expected type of a lambda expression must be a "
			"function type.");
		return nullptr;
	}
	return static_cast<SnClassDecl *>(pFuncType);
}

//0.13.10 Task 4: the expected-type channel (spec §3.2). Every error
//exit flags the node resolved — the decomposed local-decl revisit is
//the double-report discipline (Task 3's gate established it).
void ExprResolveAccessor::TypeLambda(SnLambdaExpr &sn)
{
	auto *pFuncDecl = AdmitLambdaTarget(m_Env, sn);
	if (!pFuncDecl)
	{
		sn.AddFlags(NF_Resolved);
		return;
	}
	auto *pFuncType = static_cast<SnField *>(pFuncDecl);
	auto typeArgs = GetGenericTypeArgs(pFuncDecl);
	//The FIRST type argument is the return type; the rest are the
	//parameters (nlang.y MakeFuncType's argument order).
	NLANG_ASSERT(!typeArgs.empty());
	if (typeArgs.size() - 1 != sn.Params().size())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"the lambda takes %zu parameters; the expected type \"%s\" "
			"expects %zu.",
			sn.Params().size(), pFuncType->ToString().c_str(),
			typeArgs.size() - 1);
		sn.AddFlags(NF_Resolved);
		return;
	}
	if (!FillLambdaParams(sn, *pFuncDecl))
	{
		sn.AddFlags(NF_Resolved);
		return;
	}
	//Stamp before the body walk — block-body returns read the type.
	sn.EvalDataType(pFuncType);
	sn.AddFlags(NF_Resolved);

	auto *pReturnType = typeArgs[0];
	if (sn.ExprBody())
	{
		if (pReturnType->Kind() == NK_Void)
		{
			//D6: a func<void> expectation discards nothing silently —
			//a value-yielding expression body is a compile error.
			m_Env.Log(CLL_Error, sn.ExprBody()->Location(),
				"the expected type \"%s\" returns void; the lambda's "
				"expression body yields a value.",
				pFuncType->ToString().c_str());
			return;
		}
		ResolveLambdaExprBody(sn, pReturnType);
		return;
	}
	ResolveLambdaBlockBodyChannel(sn, pReturnType);
}

//0.13.10 Task 3: the lambda resolution entry — the parameter-shape
//gate first (it reports the first offending item only), then the
//Task 4 typing. Both flag the node resolved so a decomposed local-decl
//revisit cannot re-report.
void ExprResolveAccessor::Access(SnLambdaExpr &sn)
{
	if (sn.IsResolved())
	{
		return;
	}

	if (InvalidLambdaParameterList(sn))
	{
		for (const auto &param : sn.Params())
		{
			if (ParamIsRawExpression(param))
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"invalid lambda parameter list; \"%s\" is not a "
					"parameter.",
					param.pValueExpr->ToString().c_str());
				break;
			}
		}
		sn.AddFlags(NF_Resolved);
		return;
	}

	TypeLambda(sn);
}

} //namespace nlang
