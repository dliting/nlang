/*---
    ExprResolverCast.cpp — 类型距离与隐式转换包装
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
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Phase 13: distance contribution of a pending function-reference binding
//(bare identifier or receiver-bound member form): 0 when the formal is a
//Func type whose signature matches the referenced function exactly, -1
//otherwise. The exact-match-only rule also closes the legacy hole where
//the bare name's RETURN type let it bind approximately to non-Func
//formals.
static int FuncRefBindingDistance(const FormalBinding &b)
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
	if (!IsFuncTypeDecl(pTgt) || !pRef
		|| !FuncRefMatchesDecl(*pRef, static_cast<SnClassDecl*>(pTgt)))
		return -1;
	return 0;
}

//Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
//entries. B_Default contributes 0. Returns -1 if any bound entry has
//incompatible types.
int ExprResolveAccessor::ComputeBindingDistance(
	const std::vector<FormalBinding> &bindings) const
{
	int nDistance = 0;
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		assert(b.pCallerExpr && b.pFormal);
		if (IsUnboundFuncRef(*b.pCallerExpr)
			|| IsUnboundMemberFuncRef(*b.pCallerExpr))
		{
			if (FuncRefBindingDistance(b) < 0)
				return -1;
			continue;
		}
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			return -1;
		//0.7.3 B: array-ness is adjudicated by CalcTypeDistance's kind
		//matching alone — an interned token against a scalar formal (and
		//the symmetric hole) verdicts -1 there. One exemption follows:
		//null against an array formal.
		//0.7.3 B: the null literal is Int32-typed, so against an
		//array-token formal CalcTypeDistance reads -1. Null binds to
		//any array type at distance 0 — the same bridge the assignment
		//flavor grants (`int[] a = null`; the null sentinel is not an
		//array value).
		if (b.pCallerExpr->ContainFlags(NF_NullLiteral)
			&& pTgt->Kind() == NK_ArrayTypeToken)
			continue;
		int n = CalcTypeDistance(*pSrc, *pTgt);
		if (n < 0)
		{
			LogArrayBindingReject(b);
			return -1;
		}
		//Phase 9e: out bindings require the exact same type — the callee
		//writes its slot straight back into the caller's variable; any
		//implicit cast (int→float etc.) would be discarded by writeback.
		if (b.bIsOut && n != 0)
			return -1;
		nDistance += n;
	}
	return nDistance;
}

//0.7.3 B: a cross-element array conversion (an int[] source against a
//string[] formal) verdicts -1 in CalcTypeDistance — give it the named
//array diagnostic; the caller's generic "not compatible" alone hides the
//array reason.
void ExprResolveAccessor::LogArrayBindingReject(const FormalBinding &b) const
{
	if (b.pCallerExpr->IsArrayValued())
		m_Env.Log(CLL_Error, b.pCallerExpr->Location(),
			"Invalid conversion \"%s\": an array value only converts "
			"to the same array type.",
			b.pCallerExpr->ToString().c_str());
}

//Phase 9c: apply implicit cast wrappers (SnCastExpr) to caller-side
//expressions in bindings where needed (TCK_Auto / TCK_Box). B_Default
//entries are skipped — their type was validated against the formal at
//declaration time (StatementResolver Step 2).
void ExprResolveAccessor::FixupParamTypesWithBindings(SnInvokeExpr &invoke,
	std::vector<FormalBinding> &bindings)
{
	auto &children = invoke.Children();
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		//Phase 9e: out bindings already required exact type match in
		//ComputeBindingDistance — wrapping a cast would break the
		//variable-slot identity the writeback relies on.
		if (b.bIsOut)
			continue;
		assert(b.pCallerExpr && b.pFormal);
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			continue;
		TypeCastInfo castInfo(pSrc, pTgt);
		if (castInfo.Kind() == TCK_Same)
			continue;

		//Locate the caller expr's NodeIterator inside invoke.Children().
		//For positional bindings this finds the caller expr directly;
		//for named bindings the wrapper SnNamedArgExpr holds it (helper).
		auto iFound = children.find(b.pCallerExpr);
		if (iFound == children.end())
		{
			//Named-arg path: no wrapper holds it → skip the binding.
			if (!TryFixupNamedArgBinding(invoke, b, castInfo))
				continue;
		}
		else
		{
			//FixupExprType may replace iFound with a new SnCastExpr node.
			FixupExprType(iFound, castInfo);
			//Refresh the binding's caller pointer to the (possibly new) node.
			b.pCallerExpr = static_cast<SnExpression*>(&*iFound);
		}
	}
}

//Named-arg arm of FixupParamTypesWithBindings: pCallerExpr sits inside a
//SnNamedArgExpr wrapper rather than directly in Children(). Find the
//wrapper, fix up its inner expression and repoint the binding's caller
//pointer. False = no wrapper holds this expr (binding skipped by caller).
bool ExprResolveAccessor::TryFixupNamedArgBinding(SnInvokeExpr &invoke,
	FormalBinding &b, TypeCastInfo &castInfo)
{
	auto &children = invoke.Children();
	for (auto it = children.begin(); it != children.end(); ++it)
	{
		if ((*it).Kind() != NK_NamedArgExpr)
			continue;
		auto &named = static_cast<SnNamedArgExpr&>(*it);
		if (named.Inner() != b.pCallerExpr)
			continue;
		auto &innerChildren = const_cast<SnNamedArgExpr&>(named).Children();
		auto iInner = innerChildren.find(b.pCallerExpr);
		if (iInner == innerChildren.end())
			continue;
		FixupExprType(iInner, castInfo);
		b.pCallerExpr = static_cast<SnExpression*>(&*iInner);
		return true;
	}
	return false;
}

//Class-to-class distance: walk the source's superclass chain; the depth
//where the target appears is the distance (direct superclass = 1).
static int ClassInheritanceDistance(const SnClassDecl &source,
	const SnClassDecl &target)
{
	auto *pParent = source.SuperClass();
	int depth = 1;
	while (pParent)
	{
		if (pParent == &target)
			return depth;
		pParent = pParent->SuperClass();
		++depth;
	}
	return -1;
}

//Class to interface: walk source class's inheritance chain and check
//each ancestor's implements list. Distance is 1 + inheritance depth
//(encourage upcast to direct implementor over a deeper ancestor's
//implementation, but still accept any depth).
static int ClassInterfaceDistance(const SnClassDecl &source,
	const SnInterfaceDecl &target)
{
	auto *pCur = &source;
	int depth = 0;
	while (pCur)
	{
		for (auto *pIface : pCur->ImplementsList())
			if (pIface == &target)
				return depth + 1;
		pCur = pCur->SuperClass();
		++depth;
	}
	return -1;
}

//0.7.5 overload-distance rungs for implicit primitive conversions.
//In-category and cross-sign containment widenings cost the registry
//rank delta (max 3); the integer→float hop and the →string coercion
//sit on fixed rungs above every widening, so byte→int < byte→float <
//byte→string — the direction the old abs(NK delta) scheme lost once
//the kind values were reshuffled. string coercion sits above the
//float hop (numeric formal beats string formal for a numeric arg).
static const int kDistToFloat = 4;   // > max in-category rank delta (3)
static const int kDistToString = 5;  // > kDistToFloat

//The primitive×primitive ladder (0.7.5 registry derivation). Verdicts
//come from the cast table: TCK_None and the explicit-only narrowings
//carry no candidacy for implicit binding flows (`as` never consults
//distance); Same is 0; the Auto verdicts cost their rung.
static int PrimitiveDistance(NodeKind srcKind, NodeKind tgtKind)
{
	const auto verdict = TypeCastInfo::PrimitiveVerdict(srcKind, tgtKind);
	if (verdict == TCK_None || verdict == TCK_Explicit)
		return -1;
	if (verdict == TCK_Same)
		return 0;
	if (tgtKind == NK_String)
		return kDistToString;
	const int si = ScalarPrimIndexOf(srcKind);
	const int ti = ScalarPrimIndexOf(tgtKind);
	if (si >= 0 && ti >= 0)
	{
		const auto &s = kScalarPrims[si];
		const auto &t = kScalarPrims[ti];
		if (s.category != PC_Float && t.category == PC_Float)
			return kDistToFloat;               // integer → float hop
		//Same-category chains and the cross-sign containments share
		//the rank-delta ladder (ubyte→short, ushort→int, uint→long
		//are all 1, like a one-step widening).
		return std::abs(int(s.rank) - int(t.rank));
	}
	return kDistToFloat;
}

int ExprResolveAccessor::CalcTypeDistance(const SnField &source,
	const SnField &target) const
{
	if (&source == &target)
		return 0;
	auto srcKind = source.Kind();
	auto tgtKind = target.Kind();
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	if (srcKind == NK_StructDecl && tgtKind == NK_StructDecl)
		return (&source == &target) ? 0 : -1;
	if (srcKind == NK_StructDecl || tgtKind == NK_StructDecl)
		return -1;
	if (srcKind == NK_ClassDecl && tgtKind == NK_ClassDecl)
		return ClassInheritanceDistance(
			static_cast<const SnClassDecl&>(source),
			static_cast<const SnClassDecl&>(target));
	if (srcKind == NK_ClassDecl && tgtKind == NK_InterfaceDecl)
		return ClassInterfaceDistance(
			static_cast<const SnClassDecl&>(source),
			static_cast<const SnInterfaceDecl&>(target));
	//Interface to interface: identity only (no inheritance between interfaces).
	if (srcKind == NK_InterfaceDecl && tgtKind == NK_InterfaceDecl)
		return (&source == &target) ? 0 : -1;
	//Interface to class is never valid — interface refs cannot be downcast
	//implicitly (no dynamic cast in this phase).
	if (srcKind == NK_InterfaceDecl || tgtKind == NK_InterfaceDecl)
		return -1;
	if (srcKind == NK_ClassDecl || tgtKind == NK_ClassDecl)
		return -1;
	//0.7.3 B D5: an array argument coerces to a string formal through the
	//cast table's TCK_Auto (runtime toString). Grant candidacy a finite
	//distance — the same rung as every scalar→string coercion (0.7.5:
	//the hardcoded 1 became this rung). Everything else array-token-
	//shaped stays -1: same-token pairs already returned 0 at the pointer
	//check above, cross-token and token-vs-scalar pairs have no
	//conversion.
	if (srcKind == NK_ArrayTypeToken && tgtKind == NK_String)
		return kDistToString;
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
	{
		//0.7.5: distances derive from the registry ladder above (the
		//NK reshuffle made kind arithmetic meaningless).
		return PrimitiveDistance(srcKind, tgtKind);
	}
	return -1;
}

//0.7.3 B: a TCK_None verdict between two array tokens (different
//element types) keeps the NAMED array diagnostic — the gate must sit
//before the generic reject in RejectIncompatibleCast, or
//covariant/enum-array conversions surface as the generic "Incompatible
//type". Same-type array flow returned TCK_Same earlier; an array source
//against a scalar target verdicts None without a token target and takes
//the generic message; array→string coerces via TCK_Auto (runtime
//toString dispatch, array-aware — `"${arr}"` yields "[1, 2]").
bool ExprResolveAccessor::RejectArrayTokenCast(SnExpression &srcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_None
		&& srcExpr.IsArrayValued()
		&& castInfo.Target()
		&& castInfo.Target()->Kind() == NK_ArrayTypeToken)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Invalid conversion \"%s\": an array value only converts "
			"to the same array type.",
			srcExpr.ToString().c_str());
		return true;
	}
	return false;
}

//The three incompatibility gates of FixupExprType, in their required
//order. True = a diagnostic was logged and the caller must stop.
bool ExprResolveAccessor::RejectIncompatibleCast(SnExpression &srcExpr,
	TypeCastInfo &castInfo)
{
	if (RejectArrayTokenCast(srcExpr, castInfo))
		return true;

	//0.7.5: this accept set (Same/Auto/Box) is the implicit-flow gate —
	//the new TCK_Explicit narrowing verdicts (float→int and friends)
	//are rejected here by construction, keeping every implicit path
	//assignment/argument/return free of silent narrowing.
	if (castInfo.Kind() != TCK_Auto && castInfo.Kind() != TCK_Box)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible type \"%s\".", srcExpr.ToString().c_str());
		return true;
	}

	//The int→class/interface/array bridge (TCK_Auto) exists for the null
	//literal only: any other int/enum value would end up as a garbage
	//handle in the slot (an array target doubly so — a raw int would
	//reintroduce the masquerade). ContainFlags is a flat bit test, so a
	//null literal wrapped in `as` propagates the flag explicitly in
	//Access(SnAsExpr).
	if (castInfo.Kind() == TCK_Auto
		&& castInfo.Target()
		&& (castInfo.Target()->Kind() == NK_ClassDecl
			|| castInfo.Target()->Kind() == NK_InterfaceDecl
			|| castInfo.Target()->Kind() == NK_ArrayTypeToken)
		&& !srcExpr.ContainFlags(NF_NullLiteral))
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible value \"%s\": only the null literal converts "
			"from int to a class, interface or array type.",
			srcExpr.ToString().c_str());
		return true;
	}
	return false;
}

//Null literal (KT_Null is Int32-typed) must reach the slot as the raw
//sentinel 0. Wrapping it destroys the null identity downstream:
//Int32→String emits OP_Prim_to_str ("0"), TCK_Box to Object allocates
//a boxed 0. Class/interface targets already treat TCK_Auto as a
//runtime no-op, so skipping the wrap uniformly is safe there too.
//Null operands in binary arithmetic/concat are rejected outright by
//the guard in Access(SnBinaryExpr) (Phase 11 Step 3b) — the skip here
//cannot leak a raw null into an arithmetic wrap anymore.
bool ExprResolveAccessor::SkipNullIdentityWrap(SnExpression &srcExpr,
	TypeCastInfo &castInfo)
{
	return srcExpr.ContainFlags(NF_NullLiteral)
		&& (castInfo.Kind() == TCK_Box
			|| (castInfo.Target()
				&& castInfo.Target()->Kind() == NK_String));
}

bool ExprResolveAccessor::FixupExprType(NodeIterator &iSrcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_Same)
		return false;

	assert(static_cast<SyntaxNode &>(*iSrcExpr).IsExpression());
	auto &srcExpr = static_cast<SnExpression &>(*iSrcExpr);

	if (RejectIncompatibleCast(srcExpr, castInfo))
		return false;
	if (SkipNullIdentityWrap(srcExpr, castInfo))
		return false;

	auto pSrcParent = srcExpr.Parent();
	assert(pSrcParent);

	auto iInsertPos = RemoveChildFrom(iSrcExpr, *pSrcParent);
	assert(srcExpr.Location());
	auto pCastExpr =
		new SnCastExpr(&srcExpr, castInfo, *srcExpr.Location());
	//Phase 8e-8: propagate target type to the cast expr's EvalDataType so
	//consumers (e.g. binary codegen dispatching on operand type) see the
	//post-cast type without re-walking the cast. The "as T" resolver path
	//sets this explicitly at line 709; FixupExprType must do the same.
	pCastExpr->EvalDataType(castInfo.Target());
	iSrcExpr = InsertChildInto(iInsertPos, pCastExpr, *pSrcParent);
	return true;
}

} //namespace nlang
