/*---
    ExprResolverBinding.cpp — 重载候选的绑定距离核算与选定后的实参绑定尾
    （函数引用戳绑、deferred实参绑定）。距离核算从 ExprResolverCast.cpp
    抽取（2026-10-12 source-size 拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "ExprResolverCastFit.hpp"
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
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Phase 13: distance contribution of a pending function-reference binding
//(bare identifier or receiver-bound member form): 0 when the formal is a
//func type whose signature matches the referenced function exactly, -1
//otherwise. The exact-match-only rule also closes the legacy hole where
//the unqualified name's RETURN type let it bind approximately to non-func
//formals.
static int FuncRefBindingDistance(BuildEnvironment &env,
	const FormalBinding &b)
{
	SnFunction *pRef = nullptr;
	if (IsUnboundFuncRef(*b.pCallerExpr))
		pRef = static_cast<SnFunction*>(
			static_cast<SnIdentifierExpr*>(b.pCallerExpr)->Field());
	else if (IsUnboundMemberFuncRef(*b.pCallerExpr))
		pRef = static_cast<SnFunction*>(
			static_cast<SnIdentifierExpr*>(
				static_cast<SnMemberExpr*>(
					b.pCallerExpr)->Inner())->Field());
	auto *pTgt = b.pFormal->EvalDataType();
	if (!IsFuncTypeDecl(pTgt) || !pRef)
		return -1;
	//0.13.10 Task 5: a generic reference unifies against the formal —
	//distance 0 when the expected type's slots fill every type
	//parameter (the value channel; the call-site bind re-runs the same
	//unification idempotently).
	if (pRef->IsGenericDeclaration())
	{
		std::vector<SnField*> inferredArgs;
		return UnifyGenericFuncRefAgainst(*pRef,
			static_cast<SnClassDecl*>(pTgt),
			env.Registry().DeclaredGenerics(), inferredArgs)
			? 0 : -1;
	}
	//0.13.10 Task 5: a receiverless static of a generic CLASS unifies
	//the CLASS's parameters from the formal's slots — the same channel
	//the bind site's clone arm runs. An underdetermined signature (the
	//CS0411 shape) scores -1 here; the bind site reports the
	//cannot-infer diagnostic.
	auto *pOwner = pRef->Parent();
	if (pOwner && pOwner->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pOwner)->IsGenericDeclaration()
		&& IsReceiverlessFuncRefTarget(*pRef))
	{
		std::vector<SnField*> classArgs;
		bool bUnderdetermined = false;
		return UnifyClassStaticRefAgainst(*pRef,
			*static_cast<SnClassDecl*>(pOwner),
			static_cast<SnClassDecl*>(pTgt),
			env.Registry().DeclaredGenerics(), classArgs,
			bUnderdetermined) ? 0 : -1;
	}
	if (!FuncRefMatchesDecl(*pRef, static_cast<SnClassDecl*>(pTgt)))
		return -1;
	return 0;
}

//Null-literal policy against a formal type (the null literal is
//Int32-typed): 0 = no null special case, adjudicate via CalcTypeDistance;
//1 = accept (null binds an array formal as the raw sentinel 0, same bridge
//the assignment grants); -1 = reject (string is a value type since
//0.13.4 — the null literal has no string value). Class/interface formals
//fall through to the distance calculation.
static int NullBindingVerdict(SnExpression &caller, const SnField &formal)
{
	if (!caller.ContainFlags(NF_NullLiteral))
		return 0;
	const NodeKind k = formal.Kind();
	if (k == NK_ArrayTypeToken)
		return 1;
	if (k == NK_String)
		return -1;
	return 0;
}

//0.9.0: the formal shapes a bare brace argument can infer against —
//the init-list target shapes (IsInitListTargetShape, the single
//authority in ExprResolverInitList.cpp); the formal must carry a type.
static bool AdmitsInitListArg(const SnFormalParam &formal)
{
	auto *pType = formal.EvalDataType();
	return pType && IsInitListTargetShape(pType);
}

//Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
//entries. B_Default contributes 0. Returns -1 if any bound entry has
//incompatible types.
//Fitted-constant scoring twin of the fixup-phase ConstantFitGate: a
//compile-time constant whose value fits the formal converts implicitly
//on assignment (`float f = 0.5` binds), so candidate scoring must see
//the same conversion or the overload is unreachable — a double literal
//against a float formal is TCK_Explicit, which the raw ladder grades
//-1, and a relu-shaped call (float array + 0.0 bound) would have no
//viable candidate at all. Same predicates as the gate: Explicit pair,
//scalar target, TryConstantFit says Fits.
static bool BindingConstantFits(SnExpression &caller, SnField *pTgt)
{
	TypeCastInfo castInfo(caller.EvalDataType(), pTgt);
	if (castInfo.Kind() != TCK_Explicit)
		return false;
	const int ti = pTgt ? ScalarPrimIndexOf(pTgt->Kind()) : -1;
	if (ti < 0)
		return false;
	return TryConstantFit(caller, kScalarPrims[ti]).verdict == CF_Fits;
}

//The rung a fitting constant binds with: the pair's widening ladder
//(the same table PrimitiveDistance grades) with the Explicit verdict
//read as its natural conversion. Exact-type matches stay at 0 and beat
//every fit; an out-param (distance must be 0) can never come from a
//fit because a same-type pair never verdicts Explicit.
static int FittedConstantRung(NodeKind srcKind, NodeKind tgtKind)
{
	srcKind = CastOperandKind(srcKind);
	tgtKind = CastOperandKind(tgtKind);
	const int si = ScalarPrimIndexOf(srcKind);
	const int ti = ScalarPrimIndexOf(tgtKind);
	if (si < 0 || ti < 0)
		return -1;
	const auto &s = kScalarPrims[si];
	const auto &t = kScalarPrims[ti];
	if (s.category != PC_Float && t.category == PC_Float)
		return kDistToFloat;               // integer → float hop
	return std::abs(int(s.rank) - int(t.rank));
}

//Distance of one typed (positional / named) binding: the raw ladder,
//then the fitted-constant promotion (see BindingConstantFits — a
//constant whose value fits the formal binds at its natural rung).
//-1 = incompatible.
int ExprResolveAccessor::TypedBindingDistance(SnExpression &caller,
	SnField &srcT, SnField &tgtT) const
{
	int n = CalcTypeDistance(srcT, tgtT);
	if (n >= 0)
		return n;
	if (!BindingConstantFits(caller, &tgtT))
		return -1;
	return FittedConstantRung(srcT.Kind(), tgtT.Kind());
}

//Per-binding deferral admission (the brace and lambda deferral
//contracts): 0 = deferred and admitted (no distance contribution),
//-1 = deferred and rejected, 1 = not a deferred shape.
static int DeferredArgAdmission(SnExpression &caller,
	SnFormalParam &formal)
{
	if (UntypedInitListOf(caller))
		return AdmitsInitListArg(formal) ? 0 : -1;
	if (UntypedLambdaOf(caller))
	{
		auto *pTgt = formal.EvalDataType();
		return (pTgt && IsFuncTypeDecl(pTgt)) ? 0 : -1;
	}
	return 1;
}

int ExprResolveAccessor::ComputeBindingDistance(
	const std::vector<FormalBinding> &bindings) const
{
	int nDistance = 0;
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		NLANG_ASSERT(b.pCallerExpr && b.pFormal);
		if (IsUnboundFuncRef(*b.pCallerExpr)
			|| IsUnboundMemberFuncRef(*b.pCallerExpr))
		{
			if (FuncRefBindingDistance(m_Env, b) < 0)
				return -1;
			continue;
		}
		const int nDeferred = DeferredArgAdmission(*b.pCallerExpr,
			*b.pFormal);
		if (nDeferred < 0)
			return -1;
		if (nDeferred == 0)
			continue;
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			return -1;
		//Null-literal policy (array accept / string reject) precedes the
		//generic distance; see NullBindingVerdict.
		const int nNull = NullBindingVerdict(*b.pCallerExpr, *pTgt);
		if (nNull > 0)
			continue;
		if (nNull < 0)
			return -1;
		const int n = TypedBindingDistance(*b.pCallerExpr, *pSrc, *pTgt);
		if (n < 0)
			return -1;
		//Trial scoring stays pure (TryBindArgs' no-logging contract):
		//rejecting a non-matching overload is expected among same-name
		//array overloads; the final verdict's notes name the conversion.
		nDistance += n;
	}
	return nDistance;
}

//v0.13.13a D-2(a): the final verdict for a ctor call whose actuals hold a
//pending function reference that no formal accepted — the scoring sweep
//above is silent by contract, so MatchCtorOrLogMismatch would say only
//"no constructor matches the argument list". Re-run the trial bindings
//per candidate and replay the FuncRefBindingDistance verdict for that
//reference: the (formal, ctor) that rejected it gets named. Candidates
//are scanned in declaration order (the same set order the match used);
//the first rejecting anchor wins.
bool ExprResolveAccessor::MaybeLogCtorFuncRefMismatch(
	const std::vector<SnExpression*> &actuals,
	const std::vector<SnFunction*> &candidates,
	const ISourceLocation *pLoc, const std::string &typeName)
{
	for (auto *pActual : actuals)
	{
		SnExpression *pValue = (pActual->Kind() == NK_NamedArgExpr)
			? static_cast<SnNamedArgExpr*>(pActual)->Inner() : pActual;
		if (!IsUnboundFuncRef(*pValue)
			&& !IsUnboundMemberFuncRef(*pValue))
			continue;
		for (auto *pCandidate : candidates)
		{
			std::vector<FormalBinding> tryBind;
			if (!TryBindArgs(actuals, *pCandidate, tryBind))
				continue;
			for (auto &b : tryBind)
			{
				if (b.kind == FormalBinding::B_Default
					|| b.pCallerExpr != pValue)
					continue;
				if (FuncRefBindingDistance(m_Env, b) < 0)
				{
					m_Env.Log(CLL_Error, pLoc,
						"function \"%s\" does not match parameter \"%s\" "
						"of constructor \"%s\"",
						pValue->ToString().c_str(),
						b.pFormal->Name().c_str(), typeName.c_str());
					return true;
				}
			}
		}
	}
	return false;
}

//Phase 13: the func-ref argument binder of ResolveInvokeWithFunc (moved
//here from ExprResolverInvoke.cpp in the v0.13.13a source-size split —
//this TU owns the func-ref binding verdicts). Argument-position function
//references bind against the formal func types of the chosen overload
//(ComputeBindingDistance already required an exact signature match for
//candidacy). Step 2 adds the receiver-bound member form (c.foo).
bool ExprResolveAccessor::BindArgFuncRefs(
	std::vector<FormalBinding> &bindings)
{
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default || !b.pCallerExpr)
			continue;
		if (IsUnboundFuncRef(*b.pCallerExpr))
		{
			if (!BindFuncRefToExpected(m_Env,
				*static_cast<SnIdentifierExpr*>(b.pCallerExpr),
				b.pFormal->EvalDataType()))
				return false;
		}
		else if (IsUnboundMemberFuncRef(*b.pCallerExpr)
			&& !BindMemberFuncRefToExpected(m_Env,
				*static_cast<SnMemberExpr*>(b.pCallerExpr),
				b.pFormal->EvalDataType(), this))
			return false;
	}
	return true;
}

//0.9.0: the deferred arguments of the chosen overload — the formal's
//declared type is the target (the same declaration field shape the
//assignment site passes as InferredTarget; 0.13.10: a lambda argument
//takes it as its expected function type). False when any of them
//failed to resolve; its diagnostic is already logged.
//0.9.13 B2: named arguments resolve through their SnNamedArgExpr wrapper.
//0.13.10 Task 7: a deferred argument is CALLER code — it hangs off the
//call node in the caller's tree, never in the callee's scope. The member-
//receiver path reaches this channel with the receiver-class context and
//the parent-only flag still active (its callee lookup needed them);
//anchor the resolution at the call node itself and clear the flag, so
//the argument's walk follows the caller's scope chain. The bare path
//arrives with the caller context already set — the anchor is the same
//chain, one hop shallower. v0.13.13a D-2(b): \p argsOwner is the node
//owning the actual-argument children (SnInvokeExpr, SnNewExpr — the
//constructor binding tails share this channel too).
bool ExprResolveAccessor::ResolveDeferredArgs(
	SyntaxNode &argsOwner, std::vector<FormalBinding> &bindings)
{
	SyntaxNode *pSavedContext = m_pContext;
	const bool bParentOnly = ContainFlags(ERF_SearchInParentOnly);
	m_pContext = &argsOwner;
	RemoveFlags(ERF_SearchInParentOnly);
	bool bOK = true;
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default || !b.pCallerExpr)
			continue;
		if (auto *pInitList = UntypedInitListOf(*b.pCallerExpr))
			pInitList->InferredTarget(b.pFormal);
		else if (auto *pLambda = UntypedLambdaOf(*b.pCallerExpr))
			pLambda->InferredTarget(b.pFormal);
		else
			continue;
		//0.9.13 B2: named arguments (`name = {...}`) bind through their
		//SnNamedArgExpr wrapper — the wrapper (not the inner) is accepted
		//so its Access() propagates the inner's resolution to the call
		//node's child list. Mirror TryFixupNamedArgBinding: locate the
		//wrapper that holds this inner; a positional brace arg has none.
		//A constructor call never reaches this branch — SelectCtorForArgs
		//rejects named arguments before matching.
		SnExpression *pArg = b.pCallerExpr;
		auto &children = argsOwner.Children();
		for (auto it = children.begin(); it != children.end(); ++it)
		{
			if ((*it).Kind() != NK_NamedArgExpr)
				continue;
			auto &named = static_cast<SnNamedArgExpr&>(*it);
			if (named.Inner() == b.pCallerExpr)
			{
				pArg = static_cast<SnExpression*>(&named);
				break;
			}
		}
		pArg->Accept(*m_pVisitor);
		if (!pArg->IsResolved())
			bOK = false;
	}
	m_pContext = pSavedContext;
	if (bParentOnly)
		AddFlags(ERF_SearchInParentOnly);
	return bOK;
}

} //namespace nlang
