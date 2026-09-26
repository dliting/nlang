/*---
    ExprResolverInvokeDiag.cpp — 调用失败诊断（歧义、导入桩、pending 函数引用、通用不兼容/未找到）
    从 ExprResolverInvoke.cpp 抽取（2026-09-27 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include <vector>

namespace nlang
{

//M3b: the ambiguity diagnostic of MatchInvokeAgainst — logged at the
//matching core (candidate-set independent) so the bare and
//module-qualified paths report identically.
void ExprResolveAccessor::LogAmbiguousCall(SnInvokeExpr &invoke)
{
	m_Env.Log(CLL_Error, invoke.Location(),
		"ambiguous call to function \"%s\": multiple overloads match "
		"with equal distance.",
		invoke.CalleeName().c_str());
}

//Phase 13 (Step 2, cross-module): an imported stub synthesizes its
//parameter types from the return kind, so a Func argument can never
//match — name the real reason before any generic message. True = the
//named diagnostic fired (caller skips the generic text).
bool ExprResolveAccessor::MaybeLogImportedFuncRef(SnInvokeExpr &invoke,
	bool bNameMatchedImported)
{
	if (!bNameMatchedImported)
		return false;
	for (auto &arg : invoke.Params())
	{
		SnExpression *pValue = (arg.Kind() == NK_NamedArgExpr)
			? static_cast<SnNamedArgExpr&>(arg).Inner() : &arg;
		if (IsUnboundFuncRef(*pValue)
			|| IsUnboundMemberFuncRef(*pValue)
			|| IsFuncTypeDecl(pValue->EvalDataType()))
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"cannot pass a function reference to the imported "
				"function \"%s\": parameter signatures are not "
				"serialized.", invoke.CalleeName().c_str());
			return true;
		}
	}
	return false;
}

//Phase 13: a still-pending function reference among the arguments had
//no matching Func-typed formal — sweep it with the named diagnostic
//(the generic incompatibility text would not say why). True = the named
//diagnostic fired.
bool ExprResolveAccessor::MaybeLogPendingFuncRef(SnInvokeExpr &invoke)
{
	for (auto &arg : invoke.Params())
	{
		SnExpression *pValue = (arg.Kind() == NK_NamedArgExpr)
			? static_cast<SnNamedArgExpr&>(arg).Inner() : &arg;
		if (IsUnboundFuncRef(*pValue))
		{
			m_Env.Log(CLL_Error, pValue->Location(),
				"function reference \"%s\" requires an expected "
				"function type.",
				pValue->ToString().c_str());
			return true;
		}
		if (IsUnboundMemberFuncRef(*pValue))
		{
			m_Env.Log(CLL_Error, pValue->Location(),
				"bound method reference \"%s\" requires an expected "
				"function type.",
				pValue->ToString().c_str());
			return true;
		}
	}
	return false;
}

//The failure branch of Access(SnInvokeExpr) — not-found, imported-stub
//and generic incompatibility diagnostics. Shared with the module-qualified
//call path so both surfaces report identically (spec §7 / M4).
void ExprResolveAccessor::LogInvokeFailure(SnInvokeExpr &invoke,
	FindFuncResult res, SnFunction *pCallee, bool bNameMatchedImported)
{
	assert((res == FFR_Incompatible || res == FFR_FuncNameNotFound)
		&& "failure logging takes failure results only");

	if (res == FFR_Incompatible)
	{
		//pCallee is null by contract on the imported-stub path; the flag
		//comes from the name-matched candidate scan.
		if (MaybeLogImportedFuncRef(invoke, bNameMatchedImported))
			return;
		if (MaybeLogPendingFuncRef(invoke))
			return;
		m_Env.Log(CLL_Error, invoke.Location(),
			"The function invoke \"%s\" is not compatible with the "
			"declaration.", invoke.ToString().c_str());
		if (pCallee) {
			m_Env.Log(CLL_More, pCallee->Location(),
				"See also the declaration of \"%s\".",
				pCallee->ToString().c_str());
		}
		return;
	}

	assert(res == FFR_FuncNameNotFound);
	m_Env.Log(CLL_Error, invoke.Location(),
		"The function \"%s\" does not exist or is not accessible.",
		invoke.CalleeName().c_str());
}

} //namespace nlang
