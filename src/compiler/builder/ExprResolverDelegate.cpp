/*---
    ExprResolverDelegate.cpp — 委托调用与实参-形参绑定
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "nlang/compiler/CastInfo.h"
#include "RefBindJudgment.h"
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

//Per-argument gate of BindDelegateInvoke: pending function-reference
//rebinding against the slot type, and the conversion-free admission for
//value arguments. False = the argument failed (its diagnostic is
//logged); the caller accumulates the verdict and keeps scanning, so
//every bad argument is reported. v0.13.9 phase 4: the out-marker
//agreement arm is gone — the marker itself retires at Access(SnOutArgExpr).
bool ExprResolveAccessor::CheckDelegateArgument(SnExpression &arg,
	SnField *pFormal, size_t i)
{
	if (arg.Kind() == NK_OutArgExpr)
		return false;	//already logged by the retirement diagnostic
	bool bOK = true;
	//Pending bare function references bind against the func's own
	//parameter slot type; Step 2 adds the receiver-bound member form.
	if (IsUnboundFuncRef(arg))
	{
		if (!BindFuncRefToExpected(m_Env,
			static_cast<SnIdentifierExpr&>(arg), pFormal))
			bOK = false;
	}
	else if (IsUnboundMemberFuncRef(arg))
	{
		if (!BindMemberFuncRefToExpected(m_Env,
			static_cast<SnMemberExpr&>(arg), pFormal, this))
			bOK = false;
	}
	else if (!AdmitsDelegateArgType(arg.EvalDataType(), pFormal))
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"argument %zu of the delegate call is incompatible "
			"with \"%s\".", i + 1, pFormal->Name().c_str());
		bOK = false;
	}
	return bOK;
}

//Task 8: a & mark on the key's argument position — the same MakeRef
//judgment as a direct call's & formal (spec §3.9): storage gates plus
//exact type, then the promotion mark so the caller's scalar local
//compiles to the pair's stable record. Out positions stay on the out
//path (the marks are disjoint by construction; out wins defensively,
//matching the emit order). True = rejected (diagnostic logged).
static bool RejectDelegateRefMark(BuildEnvironment &env,
	SnExpression &arg, SnField *pFormal)
{
	//Task 9: a const-marked key position binds read-only (const storage
	//aliases), matching the direct-call &-argument judgment.
	//v0.13.9 phase 4: out arguments never reach here — the retirement
	//diagnostic fires at Access(SnOutArgExpr) first.
	return RejectRefBindTarget(env, arg, pFormal, arg.Location(),
		pFormal->ContainFlags(NF_Const));
}

//0.13.10 Task 5: the delegate-call constant fit. The delegate path has
//no fixup pass, so a Double-typed float literal hits the distance-0
//wall (Double→Float is explicit-only) and the raw call would pass 8
//bytes into a 4-byte formal. The Java/C# constant-fit rule applies with
//the same gate the assignment path uses: an in-range constant PRE-wraps
//into a real conversion before the admission check (the wrap emits the
//PrimCast, never raw bits — and the check sees the converted type, so
//no doomed diagnostic is logged). An out-of-range constant keeps
//ConstantFitGate's named diagnostic instead of the generic incompatible
//one. Ref-marked slots keep the strict reject — a constant has no
//storage to alias. True = the argument is fully handled here.
bool ExprResolveAccessor::ConstantFitPreWrap(SnInvokeExpr &invoke,
	SnExpression &arg, SnField *pFormal, size_t slot,
	const std::vector<uint8> &refFlags, bool &rbRejected)
{
	if (slot + 1 < refFlags.size() && refFlags[slot + 1] != 0)
		return false;
	auto *pSrc = arg.EvalDataType();
	if (!pSrc)
		return false;
	TypeCastInfo castInfo(pSrc, pFormal);
	if (castInfo.Kind() != TCK_Explicit)
		return false;
	const auto fit = ConstantFitGate(arg, castInfo);
	if (fit == CFG_Rejected)
	{
		//The gate already logged the named out-of-range diagnostic;
		//the generic incompatible report would only repeat it.
		rbRejected = true;
		return true;
	}
	if (fit != CFG_Promoted)
		return false;
	//The wrap replaces the node inside the owning Children() list (the
	//same locate-and-replace FixupParamTypesWithBindings runs).
	auto &children = invoke.Children();
	auto iFound = children.find(&arg);
	if (iFound == children.end())
		return false;
	FixupExprType(iFound, castInfo);
	return true;
}

//Shape gates of the delegate channel: no by-name dispatch (the func
//signature has no parameter names) and exact arity. False = rejected
//with the named diagnostic.
static bool DelegateShapeOk(BuildEnvironment &env, SnInvokeExpr &invoke,
	size_t nFormals)
{
	if (HasNamedArgument(invoke))
	{
		env.Log(CLL_Error, invoke.Location(),
			"named arguments are not supported in delegate calls.");
		return false;
	}
	if (ArgCountOf(invoke) != nFormals)
	{
		env.Log(CLL_Error, invoke.Location(),
			"delegate call expects %zu argument(s), got %zu.",
			nFormals, ArgCountOf(invoke));
		return false;
	}
	return true;
}

//0.13.13a D-3: the binding body shared by both delegate-call shapes.
//pFuncDecl is the func instantiation being invoked; pDelegateField is
//the callee value's field, stored into Field() for the codegen receiver
//load — null for the postfix callee-expression shape, whose codegen
//evaluates the callee expression into the delegate slot instead (the
//shape marker there is the callee child itself).
void ExprResolveAccessor::BindDelegateInvokeAgainst(SnInvokeExpr &invoke,
	SnClassDecl *pFuncDecl, SnField *pDelegateField)
{
	const auto typeArgs = GetGenericTypeArgs(pFuncDecl);
	const auto &refFlags = GetGenericRefFlags(pFuncDecl);
	if (!DelegateShapeOk(m_Env, invoke, typeArgs.size() - 1))
		return;
	bool bOK = true;
	//Snapshot the argument pointers first: the pre-wrap's node
	//replacement erases+inserts inside the very list Params() iterates,
	//so no iterator of that list may be alive across a wrap (the Debug
	//checked iterators abort on exactly this).
	std::vector<SnExpression*> args;
	for (auto &arg : invoke.Params())
		args.push_back(&arg);
	size_t i = 0;
	for (auto *pArg : args)
	{
		auto &arg = *pArg;
		if (ConstantFitPreWrap(invoke, arg, typeArgs[i + 1], i, refFlags,
				bOK))
		{
			++i;
			continue;
		}
		if (!CheckDelegateArgument(arg, typeArgs[i + 1], i))
			bOK = false;
		else if (i + 1 < refFlags.size() && refFlags[i + 1] != 0)
		{
			if (RejectDelegateRefMark(m_Env, arg, typeArgs[i + 1]))
				bOK = false;
			else
				MarkPromotedBindTarget(arg);
		}
		++i;
	}
	if (!bOK)
		return;
	//Field() carries the delegate value — codegen detects the delegate
	//shape structurally (a resolved invoke whose Field() is not an
	//SnFunction). EvalDataType follows the func's return slot — nullptr
	//for void, set directly (ResolveFieldExprAs's PostResolveCheck
	//expects a type field here).
	invoke.m_pField = pDelegateField;
	if (typeArgs[0]->Kind() != NK_Void)
		invoke.EvalDataType(typeArgs[0]);
	invoke.AddFlags(NF_Resolved);
	BindArrayTypeToken(invoke);
}

//The named callee shape: the callee names a value field holding the
//func (FindDelegateTarget's gate admitted it), so the shared binding
//runs against that field's type with the field itself as the codegen
//receiver source.
void ExprResolveAccessor::BindDelegateInvoke(SnInvokeExpr &invoke,
	SnField *pDelegateField)
{
	BindDelegateInvokeAgainst(invoke,
		static_cast<SnClassDecl*>(pDelegateField->EvalDataType()),
		pDelegateField);
}

//Phase 9c: try to bind an invoke's actual arguments to a candidate
//callee's formal parameters. Handles positional args, named args, and
//default param expressions. Returns true if every formal is bound
//(either by caller or by default); false if any required formal is left
//unbound or a caller-side error occurs (positional after named, etc.).
//Does NOT log — caller reports a generic "not compatible" error when
//no candidate matches.
//Pass-1+2 unwrap of one actual: named (`name = expr`) or positional.
//False = this candidate is unbindable — an out actual (its retirement
//diagnostic already fired at Access(SnOutArgExpr); no candidate can
//bind it) or a positional actual after a named one. rbSeenNamed threads
//the positional-after-named rule across actuals.
static bool UnwrapInvokeActual(SnExpression &actual, SnExpression *&rpExpr,
	std::string &rsName, bool &rbIsNamed, bool &rbSeenNamed)
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
		//v0.13.9 phase 4: the out marker is retired; no candidate
		//binds it (the diagnostic already fired at resolution).
		return false;
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
//declaration order (riNextFormal advances). v0.13.9 phase 4: the out-
//marker agreement arm is gone with the marker itself.
static bool BindPositionalActual(SnExpression *pExpr,
	const SnFunction::ParamList &formals,
	std::vector<FormalBinding> &outBindings, size_t &riNextFormal)
{
	if (riNextFormal >= formals.size())
		return false;  //too many positional args
	size_t idx = riNextFormal++;
	if (outBindings[idx].pCallerExpr != nullptr)
		return false;  //should never happen (positional goes in order)
	outBindings[idx].kind = FormalBinding::B_Positional;
	outBindings[idx].pCallerExpr = pExpr;
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

//0.9.11 T6: the binding core takes the actual-argument list directly (not
//an SnInvokeExpr) — constructor matching reaches it from SnNewExpr,
//SnInvokeExpr and SnSuperCallStmt alike. The vector shape is the common
//denominator of the three argument views (all three are SnExpressionList
//child views; SnNewExpr::Args() also carries the class-name child, which
//its call site skips by identity); every call site builds the resolved
//actual pointers once per call. 0.13.11: the matching side additionally
//needs the owning node — MatchCtorAgainst passes it to
//FixupParamTypesWithBindings, which splices the conversion wrappers into
//that node's children.
bool ExprResolveAccessor::TryBindArgs(const std::vector<SnExpression*> &actuals,
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

	//Pass 1+2: walk actuals left-to-right; route each to positional or named.
	for (auto *pActual : actuals)
	{
		SnExpression *pExpr;
		std::string sName;
		bool bIsNamed = false;
		if (!UnwrapInvokeActual(*pActual, pExpr, sName, bIsNamed,
				bSeenNamed))
			return false;
		if (!bIsNamed)
		{
			if (!BindPositionalActual(pExpr, formals,
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
