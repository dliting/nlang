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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

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
		//Phase 13: a pending function reference binds only to a Func
		//formal whose signature matches exactly (distance 0). This also
		//closes the legacy hole where the bare name's RETURN type let it
		//bind approximately to non-Func formals. Step 2 adds the
		//receiver-bound member form (same exact-match-only rule).
		if (IsUnboundFuncRef(*b.pCallerExpr))
		{
			auto *pTgt = b.pFormal->EvalDataType();
			auto *pRefFunc = static_cast<SnIdentifierExpr*>(
				b.pCallerExpr)->Field();
			if (!IsFuncTypeDecl(pTgt) || !pRefFunc
				|| !FuncRefMatchesDecl(
					*static_cast<SnFunction*>(pRefFunc),
					static_cast<SnClassDecl*>(pTgt)))
				return -1;
			continue;
		}
		if (IsUnboundMemberFuncRef(*b.pCallerExpr))
		{
			auto *pTgt = b.pFormal->EvalDataType();
			auto *pMethod = static_cast<SnIdentifierExpr*>(
				static_cast<SnMemberExpr*>(
					b.pCallerExpr)->Inner())->Field();
			if (!IsFuncTypeDecl(pTgt) || !pMethod
				|| !FuncRefMatchesDecl(
					*static_cast<SnFunction*>(pMethod),
					static_cast<SnClassDecl*>(pTgt)))
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
			//0.7.3 B: a cross-element array conversion (an int[] source
			//against a string[] formal) verdicts -1 here — give it the
			//named array diagnostic; the caller's generic "not
			//compatible" alone hides the array reason.
			if (b.pCallerExpr->IsArrayValued())
				m_Env.Log(CLL_Error, b.pCallerExpr->Location(),
					"Invalid conversion \"%s\": an array value only "
					"converts to the same array type.",
					b.pCallerExpr->ToString().c_str());
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
		//For positional bindings this finds the caller expr directly.
		//For named bindings, the wrapper SnNamedArgExpr is in Children();
		//its inner expr is replaced below by walking the wrapper's list.
		auto iFound = children.find(b.pCallerExpr);
		if (iFound == children.end())
		{
			//Named-arg path: pCallerExpr is inside a SnNamedArgExpr wrapper.
			//Find the wrapper, then fix up its inner expression.
			bool bReplaced = false;
			for (auto it = children.begin(); it != children.end(); ++it)
			{
				if ((*it).Kind() == NK_NamedArgExpr)
				{
					auto &named = static_cast<SnNamedArgExpr&>(*it);
					if (named.Inner() == b.pCallerExpr)
					{
						auto &innerChildren =
							const_cast<SnNamedArgExpr&>(named).Children();
						auto iInner = innerChildren.find(b.pCallerExpr);
						if (iInner == innerChildren.end())
							continue;
						FixupExprType(iInner, castInfo);
						b.pCallerExpr =
							static_cast<SnExpression*>(&*iInner);
						bReplaced = true;
						break;
					}
				}
			}
			if (!bReplaced)
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
	{
		if (&source == &target)
			return 0;
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pParent = pSrc->SuperClass();
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
	if (srcKind == NK_ClassDecl && tgtKind == NK_InterfaceDecl)
	{
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pCur = pSrc;
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
	//distance — the same magnitude as the cheapest scalar-to-string
	//widening. Everything else array-token-shaped stays -1: same-token
	//pairs already returned 0 at the pointer check above, cross-token
	//and token-vs-scalar pairs have no conversion.
	if (srcKind == NK_ArrayTypeToken && tgtKind == NK_String)
		return 1;
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
		return std::abs(srcKind - tgtKind);
	return -1;
}

bool ExprResolveAccessor::FixupExprType(NodeIterator &iSrcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_Same)
		return false;

	assert(static_cast<SyntaxNode &>(*iSrcExpr).IsExpression());
	auto &srcExpr = static_cast<SnExpression &>(*iSrcExpr);

	//0.7.3 B: a TCK_None verdict between two array tokens (different
	//element types) keeps the NAMED array diagnostic — the branch must
	//sit before the generic reject below, or covariant/enum-array
	//conversions surface as the generic "Incompatible type". Same-type
	//array flow returned TCK_Same above; an array source against a
	//scalar target verdicts None without a token target and takes the
	//generic message; array→string coerces via TCK_Auto (runtime
	//toString dispatch, array-aware — `"${arr}"` yields "[1, 2]").
	if (castInfo.Kind() == TCK_None
		&& srcExpr.IsArrayValued()
		&& castInfo.Target()
		&& castInfo.Target()->Kind() == NK_ArrayTypeToken)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Invalid conversion \"%s\": an array value only converts "
			"to the same array type.",
			srcExpr.ToString().c_str());
		return false;
	}

	if (castInfo.Kind() != TCK_Auto && castInfo.Kind() != TCK_Box)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible type \"%s\".", srcExpr.ToString().c_str());
		return false;
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
		return false;
	}

	//Null literal (KT_Null is Int32-typed) must reach the slot as the raw
	//sentinel 0. Wrapping it destroys the null identity downstream:
	//Int32→String emits OP_Int32_to_str ("0"), TCK_Box to Object allocates
	//a boxed 0. Class/interface targets already treat TCK_Auto as a
	//runtime no-op, so skipping the wrap uniformly is safe there too.
	//Null operands in binary arithmetic/concat are rejected outright by
	//the guard in Access(SnBinaryExpr) (Phase 11 Step 3b) — the skip here
	//cannot leak a raw null into an arithmetic wrap anymore.
	if (srcExpr.ContainFlags(NF_NullLiteral)
		&& (castInfo.Kind() == TCK_Box
			|| (castInfo.Target()
				&& castInfo.Target()->Kind() == NK_String)))
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
