/*---
    StatementResolverReturn.cpp — return语句解析与收敛（从StatementResolverAssign.cpp按语句族抽出，0.13.10）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//0.9.11 D1: a value return against a return-type-less function (the
//constructor gets its own wording; declaration in StatementResolver.h).
void StatementResolveAccessor::RejectValueReturnWithoutType(
	SnFunction &outerFunc, SnExpression &resultExpr)
{
	if (IsConstructorFunction(outerFunc))
		m_Env.Log(CLL_Error, resultExpr.Location(),
			"constructor \"%s\" cannot return a value",
			outerFunc.Name().c_str());
	else
		m_Env.Log(CLL_Error, resultExpr.Location(),
			"No value should be returned here.");
}

void StatementResolveAccessor::Access(SnReturnStmt &sn)
{
	NLANG_ASSERT(m_pVisitor);
	NLANG_ASSERT(m_pCurrType && m_pCurrType->Kind() == NK_Function);
	if (m_inFinallyBody) {
		m_Env.Log(CLL_Error, sn.Location(),
			"return is not allowed inside a finally block");
		return;
	}
	auto pOuterFunc = static_cast<SnFunction *>(m_pCurrType);
	auto pResultExpr = sn.Result();

	//0.13.10: a return inside a lambda block checks against the
	//lambda's stamped return type.
	if (m_pCurrLambda)
	{
		ResolveLambdaReturn(sn, *m_pCurrLambda);
		return;
	}

	auto pReturnType = pOuterFunc->ReturnType();
	if (!pResultExpr)
	{
		if (pReturnType)
			m_Env.Log(CLL_Error, sn.Location(),
				"Missing return value in a return statement.");
		return;
	}

	if (!pReturnType)
	{
		RejectValueReturnWithoutType(*pOuterFunc, *pResultExpr);
		return;
	}

	//Deferred shapes take the declared return type as their target.
	if (auto *pInitList = UntypedInitListOf(*pResultExpr))
		pInitList->InferredTarget(pOuterFunc->EvalDataType());
	else if (auto *pLambda = UntypedLambdaOf(*pResultExpr))
		pLambda->InferredTarget(pOuterFunc->EvalDataType());

	pResultExpr->Accept(*m_pVisitor);
	//v0.13.9 phase 3: an &-typed return needs storage that outlives the
	//call — a local's write lands in an orphan, so the shape rejects.
	if (pReturnType->ContainFlags(NF_Ref)
		&& RejectRefReturnTarget(*pOuterFunc, *pResultExpr,
			sn.Location()))
		return;
	FinishReturnExpr(*pOuterFunc, sn);
}

//Shared tail of Access(SnReturnStmt&): the return-position function
//reference binding (Phase 13, TryBindStatementFuncRef) and the cast-
//fixup choke point against the declared return type. Returns false when
//the binding failed (its diagnostic is logged) — the caller just stops.
bool StatementResolveAccessor::FinishReturnExpr(SnFunction &outerFunc,
	SnReturnStmt &sn)
{
	auto pResultExpr = sn.Result();
	if (!TryBindStatementFuncRef(*pResultExpr,
		outerFunc.EvalDataType()))
		return false;

	if (!outerFunc.ReturnType()->IsResolved())
		return false;
	auto pSourceType = pResultExpr->EvalDataType();
	//A result with no resolved type is already an error upstream (a
	//failed invoke match has its own named diagnosis); the cast report
	//would only bury it.
	if (!pSourceType)
		return true;
	auto pTargetType = outerFunc.EvalDataType();
	//0.7.3 B: array results and array return types both carry the
	//interned token, so the cast choke point adjudicates — same
	//token = Same (pass-through), different tokens = the named
	//array reject in FixupExprType, scalar targets = the generic
	//reject, string targets = Auto toString coercion.
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pResult);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pResult = &static_cast<SnCastExpr &>(*iExpr);
	return true;
}

//0.13.10: one return statement inside a lambda block — the function
//path's mirror against the LAMBDA's stamped return type (the first
//type argument of the stamped func instantiation). Every shape reuse:
//bare return under a non-void type, a value under func<void>, the
//nested-lambda target push, and the cast choke point.
void StatementResolveAccessor::ResolveLambdaReturn(SnReturnStmt &sn,
	SnLambdaExpr &lambda)
{
	NLANG_ASSERT(m_pVisitor);
	auto *pFuncType = lambda.EvalDataType();
	NLANG_ASSERT(pFuncType && IsFuncTypeDecl(pFuncType));
	auto typeArgs = GetGenericTypeArgs(
		static_cast<SnClassDecl *>(pFuncType));
	NLANG_ASSERT(!typeArgs.empty());
	auto *pReturnType = typeArgs[0];
	const bool bVoidReturn = pReturnType->Kind() == NK_Void;
	auto pResultExpr = sn.Result();

	if (!pResultExpr)
	{
		if (!bVoidReturn)
			m_Env.Log(CLL_Error, sn.Location(),
				"Missing return value in a return statement.");
		return;
	}
	if (bVoidReturn)
	{
		m_Env.Log(CLL_Error, pResultExpr->Location(),
			"the expected type \"%s\" returns void; this return "
			"statement carries a value.",
			pFuncType->ToString().c_str());
		return;
	}
	//A nested lambda result takes the return type as its expected
	//function type — the same identity the choke point reads.
	if (auto *pInner = UntypedLambdaOf(*pResultExpr))
		pInner->InferredTarget(pReturnType);
	pResultExpr->Accept(*m_pVisitor);
	//The never-returns bookkeeping: a resolved value return marks the
	//lambda (the typing arm reads it after the block walk).
	lambda.SetBlockHasValueReturn();
	if (!pReturnType->IsResolved())
		return;
	auto pSourceType = pResultExpr->EvalDataType();
	if (!pSourceType)
		return;
	auto castInfo = GetCastInfo(pSourceType, pReturnType);
	auto iExpr = sn.Children().find(sn.m_pResult);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pResult = &static_cast<SnCastExpr &>(*iExpr);
}

} //namespace nlang
