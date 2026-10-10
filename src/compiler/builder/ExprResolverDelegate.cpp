/*---
    ExprResolverDelegate.cpp — 委托调用与实参-形参绑定
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
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
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

SnField *ExprResolveAccessor::FindDelegateTarget(SnInvokeExpr &invoke)
{
	auto *pField = FindFieldInAncestor(invoke.CalleeName(), *m_pContext,
		*m_pAccessor, Flags());
	if (!pField || pField->Kind() == NK_Function)
		return nullptr;
	return IsFuncTypeDecl(pField->EvalDataType()) ? pField : nullptr;
}

//0.7.3 B review fix: distance alone is a wrong admission test here —
//unlike the overload path, a delegate call has NO fixup pass, so any
//accepted distance that implies a conversion (int→float, array→string
//through the D5 arm) passes raw bits and the callee reads garbage.
//Distance 0 covers identical types, interned tokens and the enum/int32
//masquerade; the only sound non-zero distances are reference upcasts
//(subclass→base, class→interface), where the handle passes through
//unchanged.
bool ExprResolveAccessor::AdmitsDelegateArgType(SnField *pArgType,
	SnField *pFormal) const
{
	if (!pArgType)
		return false;
	const bool bRefUpcast = (pArgType->Kind() == NK_ClassDecl
		|| pArgType->Kind() == NK_InterfaceDecl)
		&& (pFormal->Kind() == NK_ClassDecl
			|| pFormal->Kind() == NK_InterfaceDecl);
	const int nDist = CalcTypeDistance(*pArgType, *pFormal);
	return nDist == 0 || (bRefUpcast && nDist > 0);
}

//Per-argument gate of BindDelegateInvoke: out-marker agreement with the
//func slot, pending function-reference rebinding against the slot type,
//and the conversion-free admission for value arguments. False = the
//argument failed (its diagnostic is logged); the caller accumulates the
//verdict and keeps scanning, so every bad argument is reported.
bool ExprResolveAccessor::CheckDelegateArgument(SnExpression &arg,
	SnField *pFormal, bool bWantOut, size_t i)
{
	bool bIsOut = arg.Kind() == NK_OutArgExpr;
	SnExpression *pValue = bIsOut
		? static_cast<SnOutArgExpr&>(arg).Inner() : &arg;
	bool bOK = true;
	if (bIsOut != bWantOut)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"argument %zu of the delegate call %s the out marker.",
			i + 1, bWantOut ? "requires" : "does not accept");
		bOK = false;
	}
	//Pending bare function references bind against the func's own
	//parameter slot type; Step 2 adds the receiver-bound member form.
	if (IsUnboundFuncRef(*pValue))
	{
		if (!BindFuncRefToExpected(m_Env,
			static_cast<SnIdentifierExpr&>(*pValue), pFormal))
			bOK = false;
	}
	else if (IsUnboundMemberFuncRef(*pValue))
	{
		if (!BindMemberFuncRefToExpected(m_Env,
			static_cast<SnMemberExpr&>(*pValue), pFormal))
			bOK = false;
	}
	else if (!bIsOut && !AdmitsDelegateArgType(pValue->EvalDataType(),
		pFormal))
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"argument %zu of the delegate call is incompatible "
			"with \"%s\".", i + 1, pFormal->Name().c_str());
		bOK = false;
	}
	return bOK;
}

void ExprResolveAccessor::BindDelegateInvoke(SnInvokeExpr &invoke,
	SnField *pDelegateField)
{
	auto *pFuncDecl = static_cast<SnClassDecl*>(
		pDelegateField->EvalDataType());
	const auto typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &outFlags = GetGenericOutFlags(pFuncDecl);
	//The func signature has no parameter names — by-name dispatch is
	//impossible.
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"named arguments are not supported in delegate calls.");
		return;
	}
	size_t paramCount = typeArgs.size() - 1;
	if (ArgCountOf(invoke) != paramCount)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"delegate call expects %zu argument(s), got %zu.",
			paramCount, ArgCountOf(invoke));
		return;
	}
	bool bOK = true;
	size_t i = 0;
	for (auto &arg : invoke.Params())
	{
		if (!CheckDelegateArgument(arg, typeArgs[i + 1],
			outFlags[i + 1] != 0, i))
			bOK = false;
		++i;
	}
	if (!bOK)
		return;
	//Field() carries the delegate value — codegen detects the delegate
	//shape structurally (a resolved invoke whose Field() is not an
	//SnFunction), so no dedicated flag exists. EvalDataType follows the
	//func's return slot — nullptr for void, the established void-invoke
	//convention. Set directly (like the generic-method path) rather than
	//through ResolveFieldExprAs, whose PostResolveCheck expects a type
	//field here.
	invoke.m_pField = pDelegateField;
	if (typeArgs[0]->Kind() != NK_Void)
		invoke.EvalDataType(typeArgs[0]);
	invoke.AddFlags(NF_Resolved);
	BindArrayTypeToken(invoke);
}

//Phase 9c: try to bind an invoke's actual arguments to a candidate
//callee's formal parameters. Handles positional args, named args, and
//default param expressions. Returns true if every formal is bound
//(either by caller or by default); false if any required formal is left
//unbound or a caller-side error occurs (positional after named, etc.).
//Does NOT log — caller reports a generic "not compatible" error when
//no candidate matches.
//Pass-1+2 unwrap of one actual: named (`name = expr`), out (`out ident`)
//or positional. False = this candidate is unbindable — an unresolved out
//inner (Access(SnOutArgExpr) already logged why) or a positional actual
//after a named one. rbSeenNamed threads the positional-after-named rule
//across actuals.
static bool UnwrapInvokeActual(SnExpression &actual, SnExpression *&rpExpr,
	std::string &rsName, bool &rbIsNamed, bool &rbIsOut, bool &rbSeenNamed)
{
	if (actual.Kind() == NK_NamedArgExpr)
	{
		auto &named = static_cast<const SnNamedArgExpr&>(actual);
		rsName = named.Name();
		rpExpr = named.Inner();
		rbIsNamed = true;
		rbSeenNamed = true;
	}
	else if (actual.Kind() == NK_OutArgExpr)
	{
		//Phase 9e: out argument. Unwrap to the inner identifier; the
		//binding must land on an NF_Out formal (checked below). Named
		//+out (`foo(b = out y)`) has no grammar form.
		auto &outArg = static_cast<const SnOutArgExpr&>(actual);
		rpExpr = outArg.Inner();
		if (!rpExpr->IsResolved())
			return false;
		rbIsOut = true;
	}
	else
	{
		rpExpr = &actual;
		if (rbSeenNamed)
		{
			//"positional after named" is a caller-side error — reject
			//this candidate (caller will get a generic incompatible
			//error from FindFuncByInvoke). Phase 9c Step 4 will report
			//a specific message once grammar accepts named args.
			return false;
		}
	}
	return true;
}

//Positional routing of one actual: consumes the next formal slot in
//declaration order (riNextFormal advances) and enforces the Phase 9e
//out-marker agreement — an out formal requires `out ident` at the call
//site, and `out` is invalid for a normal formal; both directions reject
//the candidate (generic incompatibility from FindFuncByInvoke).
static bool BindPositionalActual(SnExpression *pExpr, bool bIsOut,
	const SnFunction::ParamList &formals,
	std::vector<FormalBinding> &outBindings, size_t &riNextFormal)
{
	if (riNextFormal >= formals.size())
		return false;  //too many positional args
	size_t idx = riNextFormal++;
	if (outBindings[idx].pCallerExpr != nullptr)
		return false;  //should never happen (positional goes in order)
	if (outBindings[idx].pFormal->ContainFlags(NF_Out) != bIsOut)
		return false;
	outBindings[idx].kind = FormalBinding::B_Positional;
	outBindings[idx].pCallerExpr = pExpr;
	outBindings[idx].bIsOut = bIsOut;
	return true;
}

//Named routing of one actual: binds the formal with the matching name.
//A missing name or a duplicate binding (positional+named or named+named)
//rejects the candidate.
static bool BindNamedActual(SnExpression *pExpr, const std::string &sName,
	const SnFunction::ParamList &formals,
	std::vector<FormalBinding> &outBindings)
{
	size_t idx = formals.size();
	size_t i = 0;
	for (auto &f : formals)
	{
		if (f.Name() == sName)
		{
			idx = i;
			break;
		}
		++i;
	}
	if (idx == formals.size())
		return false;  //no formal with this name
	if (outBindings[idx].pCallerExpr != nullptr)
		return false;  //duplicate binding (positional+named or named+named)
	outBindings[idx].kind = FormalBinding::B_Named;
	outBindings[idx].pCallerExpr = pExpr;
	return true;
}

//Pass 3: every formal must be either caller-bound or carry a default
//expression; unbound ones are marked B_Default here. False = a required
//formal is left unsatisfied.
static bool SatisfyUnboundFormals(std::vector<FormalBinding> &outBindings)
{
	for (auto &b : outBindings)
	{
		if (b.pCallerExpr == nullptr)
		{
			//Unbound — must have default expression.
			if (!b.pFormal->Value())
				return false;  //required formal left unsatisfied
			b.kind = FormalBinding::B_Default;
		}
	}
	return true;
}

bool ExprResolveAccessor::TryBindInvoke(const SnInvokeExpr &invoke,
	const SnFunction &callee, std::vector<FormalBinding> &outBindings)
{
	auto &formals = const_cast<SnFunction&>(callee).Params();
	outBindings.clear();
	outBindings.reserve(formals.size());
	for (auto &f : formals)
	{
		FormalBinding b;
		b.kind = FormalBinding::B_Default;  //sentinel: "unbound so far"
		b.pCallerExpr = nullptr;
		b.pFormal = &f;
		outBindings.push_back(b);
	}

	bool bSeenNamed = false;
	size_t iNextFormal = 0;
	auto &actuals = const_cast<SnInvokeExpr&>(invoke).Params();

	//Pass 1+2: walk actuals left-to-right; route each to positional or named.
	for (auto &actual : actuals)
	{
		SnExpression *pExpr;
		std::string sName;
		bool bIsNamed = false;
		bool bIsOut = false;
		if (!UnwrapInvokeActual(actual, pExpr, sName, bIsNamed, bIsOut,
				bSeenNamed))
			return false;
		if (!bIsNamed)
		{
			if (!BindPositionalActual(pExpr, bIsOut, formals,
					outBindings, iNextFormal))
				return false;
		}
		else if (!BindNamedActual(pExpr, sName, formals, outBindings))
			return false;
	}

	//Pass 3: every formal must be either caller-bound or have a default.
	return SatisfyUnboundFormals(outBindings);
}

} //namespace nlang
