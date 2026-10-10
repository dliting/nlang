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
#include "CastInfo.h"
#include <string>
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
//parameter types from the return kind, so a func argument can never
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
//no matching func-typed formal — sweep it with the named diagnostic
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
	FindFuncResult res, SnFunction *pCallee, bool bNameMatchedImported,
	const std::vector<SnFunction*> &candidates)
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
		//Per-argument notes name the actual vs expected type so the user
		//sees which argument is wrong (void result, non-printable value,
		//scalar/array mismatch) instead of only the generic summary.
		MaybeLogArgumentMismatch(invoke, candidates);
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

//Two-gate implicit-binding test; see the header note for why both the
//distance and the cast kind are required (each alone has a blind spot).
bool ExprResolveAccessor::CanImplicitlyBind(const SnField &argType,
	const SnField &formalType) const
{
	if (CalcTypeDistance(argType, formalType) < 0)
		return false;
	TypeCastInfo ci(const_cast<SnField*>(&argType),
		const_cast<SnField*>(&formalType));
	const TypeCastKind k = ci.Kind();
	return k == TCK_Same || k == TCK_Auto || k == TCK_Box;
}

//One argument's mismatch note. A string formal implicitly takes
//int/float/array (CanImplicitlyBind admits them), so a value rejected
//against it is a class/interface/enum — point it at .toString(); every
//other formal reports the actual vs expected type.
void ExprResolveAccessor::LogArgumentTypeMismatch(SnExpression &arg,
	const SnField &argType, const SnField &formalType,
	const std::string &callee, int paramIdx)
{
	if (formalType.Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"Argument %d of \"%s\" has type \"%s\"; call .toString() "
			"first to pass a class, interface or enum value.",
			paramIdx + 1, callee.c_str(), argType.ToString().c_str());
		return;
	}
	m_Env.Log(CLL_Error, arg.Location(),
		"Argument %d of \"%s\" has type \"%s\"; \"%s\" expected.",
		paramIdx + 1, callee.c_str(), argType.ToString().c_str(),
		formalType.ToString().c_str());
}

//Among name-matched candidates, return the first whose arguments route
//(the structural bind passes); its formals are the declaration reference
//for the per-argument comparison. nullptr when none routes.
SnFunction *ExprResolveAccessor::SelectRoutableCandidate(
	SnInvokeExpr &invoke, const std::vector<SnFunction*> &candidates,
	std::vector<FormalBinding> &outBindings)
{
	for (auto *pCandidate : candidates)
	{
		if (TryBindInvoke(invoke, *pCandidate, outBindings))
			return pCandidate;
	}
	return nullptr;
}

//Log one argument's mismatch note: a void result, a null literal for a
//string formal, or a type that cannot implicitly bind. True when logged.
bool ExprResolveAccessor::LogSingleArgumentMismatch(SnExpression &arg,
	const SnFormalParam &formal, const std::string &callee, int paramIdx)
{
	const SnField *pArgType = arg.EvalDataType();
	const SnField *pFormalType = formal.EvalDataType();
	if (!pArgType)
	{
		//A resolved call with no type returned void; it has no value to
		//place in the argument slot.
		m_Env.Log(CLL_Error, arg.Location(),
			"Argument %d of \"%s\" has no value: a void function result "
			"cannot be used as an argument.",
			paramIdx + 1, callee.c_str());
		return true;
	}
	if (arg.ContainFlags(NF_NullLiteral) && pFormalType
		&& pFormalType->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"Argument %d of \"%s\" cannot be null: a null value would be "
			"printed as \"0\".",
			paramIdx + 1, callee.c_str());
		return true;
	}
	if (pFormalType && !CanImplicitlyBind(*pArgType, *pFormalType))
	{
		LogArgumentTypeMismatch(arg, *pArgType, *pFormalType,
			callee, paramIdx);
		return true;
	}
	return false;
}

bool ExprResolveAccessor::MaybeLogArgumentMismatch(SnInvokeExpr &invoke,
	const std::vector<SnFunction*> &candidates)
{
	std::vector<FormalBinding> refBindings;
	if (!SelectRoutableCandidate(invoke, candidates, refBindings))
		return false;
	const std::string callee = invoke.CalleeName();
	bool bLogged = false;
	int paramIdx = 0;
	for (auto &b : refBindings)
	{
		if (b.kind != FormalBinding::B_Default && b.pCallerExpr)
			bLogged = LogSingleArgumentMismatch(*b.pCallerExpr,
				*b.pFormal, callee, paramIdx) || bLogged;
		++paramIdx;
	}
	return bLogged;
}

} //namespace nlang
