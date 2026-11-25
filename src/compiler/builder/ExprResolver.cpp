#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/core/CompiledModule.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Round-12: built-in methods dispatch by name with no real SnFunction, so a
//name = value argument can never bind to a parameter — reject it here or
//codegen's fallback would silently stage the value as ConstZero.
bool HasNamedArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_NamedArgExpr)
			return true;
	return false;
}

size_t ArgCountOf(SnInvokeExpr& invoke)
{
	size_t n = 0;
	for (auto& p : invoke.Params()) ++n;
	return n;
}

//Round-14: mirror of HasNamedArgument — built-in by-name dispatch also
//cannot write back out arguments (intrinsics return through pResult only).
bool HasOutArgument(SnInvokeExpr& invoke)
{
	for (auto& p : invoke.Params())
		if (p.Kind() == NK_OutArgExpr)
			return true;
	return false;
}

//Module import visibility (spec §6.2 rule 5): join dotted path segments.
std::string JoinDots(const std::vector<std::string>& segs)
{
	std::string joined;
	for (size_t i = 0; i < segs.size(); ++i)
	{
		if (i)
			joined += '.';
		joined += segs[i];
	}
	return joined;
}

//Inverse of JoinDots: split a dotted path into its segments. A name with no
//dot yields the one-element path; the empty name yields the empty path.
std::vector<std::string> SplitDots(const std::string& dotted)
{
	std::vector<std::string> segs;
	if (dotted.empty())
		return segs;
	size_t start = 0;
	for (;;)
	{
		const size_t dot = dotted.find('.', start);
		segs.push_back(dotted.substr(start,
			dot == std::string::npos ? std::string::npos
				: dot - start));
		if (dot == std::string::npos)
			return segs;
		start = dot + 1;
	}
}

//Collect the identifier names of a member chain's OUTER side, leftmost
//first: for `a.b.c(...)` on the member whose Inner is the invoke this is
//{a, b} (the module path). Empty when any link is not a plain identifier
//(a receiver value, a call result) — such a chain is outside the module
//fallback's contract.
std::vector<std::string> OuterIdentifierChain(
	const SnMemberExpr& snMember)
{
	std::vector<std::string> chain;  //collected inner-to-outer, reversed below
	const SyntaxNode* pLink = snMember.Outer();
	while (pLink && pLink->Kind() == NK_MemberExpr)
	{
		const auto& rLink = static_cast<const SnMemberExpr&>(*pLink);
		const SyntaxNode* pLinkInner = rLink.Inner();
		if (!pLinkInner || pLinkInner->Kind() != NK_IdentifierExpr)
			return {};
		chain.push_back(
			static_cast<const SnIdentifierExpr*>(pLinkInner)->Name());
		pLink = rLink.Outer();
	}
	if (!pLink || pLink->Kind() != NK_IdentifierExpr)
		return {};
	chain.push_back(static_cast<const SnIdentifierExpr*>(pLink)->Name());
	std::reverse(chain.begin(), chain.end());
	return chain;
}

//Phase 13: true when the field is a synthetic func<...> instantiation.
bool IsFuncTypeDecl(SnField *pType)
{
	return pType && pType->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pType)->IsFuncType();
}

//True when `expr` is a plain field lvalue (identifier, or member access
//whose inner name is an identifier) — the shapes whose Field() binding
//IS the variable/field itself. Pure SHAPE test (array redesign B):
//array-ness is the IsArrayValued() property, not part of this check.
bool IsPlainLvalueShape(const SnExpression& expr) {
	if (expr.Kind() == NK_IdentifierExpr)
		return true;
	if (expr.Kind() == NK_MemberExpr) {
		auto* pInner = static_cast<const SnMemberExpr&>(expr).Inner();
		return pInner && pInner->Kind() == NK_IdentifierExpr;
	}
	return false;
}

//0.7.3 B: the stamp became a TOKEN BINDER. The array-valued property
//is now derived (IsArrayValued reads the interned SnArrayTypeToken in
//EvalDataType). Every shape except a fresh allocation carries the
//token from ResolveFieldExprAs already: identifiers and plain members
//bind the declaration's token, user-method invokes bind the return
//type's token, and container element flows (List<T[]>.get, li[0],
//func<R[],...> invokes) read the instantiation's type-args slots —
//which carry tokens natively because generic type arguments resolve
//through the same Access(SnArrayTypeExpr&) intern channel. Creating
//over any of those would wrap a token in a token (the double-wrap bug
//the gc-arm tests catch). Still called at every NF_Resolved success
//site — a new value shape that is array-valued without passing a
//declaration channel needs an arm here, or it silently loses its
//array-ness.
void ExprResolveAccessor::BindArrayTypeToken(SnExpression& expr) {
	if (!expr.IsResolved())
		return;
	//Creation shapes only: `new T[n]` and the 0.9.0 twin `T[n]`
	//(annotated with NewArrayElem). Every other shape resolves through
	//a declaration channel that already carries the token.
	const bool isCreation = expr.Kind() == NK_NewArrayExpr
		|| (expr.Kind() == NK_SubscriptExpr
			&& static_cast<SnSubscriptExpr&>(expr).NewArrayElem() != nullptr);
	if (!isCreation)
		return;
	//The creation resolve tail stored the element field in
	//EvalDataType — create the token over it. The never-wrap check stays:
	//an element field that is already a token must not be wrapped again.
	auto* pElem = expr.EvalDataType();
	if (pElem && pElem->Kind() != NK_ArrayTypeToken)
		expr.EvalDataType(m_Env.InternArrayTypeToken(pElem));
}

bool ExprResolveAccessor::ResolveExpressionList(SnExpressionList &exprs,
	const std::vector<SnExpression*> *pDeferredArgs)
{
	bool bOK = true;
	for (auto &expr : exprs)
	{
		//Skip already-resolved expressions: literals come pre-resolved
		//from parse time, and Access(SnMemberExpr) pre-resolves method
		//invoke args in the caller scope before the callee lookup runs
		//in the receiver-class scope. Deferred bare brace arguments skip
		//until the callee lookup hands them their formal type.
		if (pDeferredArgs
			&& std::find(pDeferredArgs->begin(), pDeferredArgs->end(),
				&expr) != pDeferredArgs->end())
			continue;
		if (!expr.IsResolved())
			expr.Accept(*m_pVisitor);
		if (!expr.IsResolved() && bOK)
			bOK = false;
	}
	//0.13.1c identity rescue: splice staged rescues now — the pass over
	//this list is done, so no iterator is live — then recompute the
	//result over the repaired children (the misread node failed
	//silently by design and is gone by now; siblings that failed with
	//real diagnostics stay unresolved and keep the list failed).
	if (!m_rescueStash.empty())
	{
		ApplyRescueStash(exprs);
		bOK = true;
		for (auto &expr : exprs)
		{
			if (!expr.IsResolved())
			{
				bOK = false;
				break;
			}
		}
	}
	return bOK;
}

//0.13.1c identity rescue: apply the staged splices targeting this list.
//The misread creation node is detached and destroyed; its rescued
//arguments take its place in the owner's children (an argument-count
//change is fine — the call's arguments were parsed positionally) and
//resolve through the normal expression path, so a rescued comparison
//that fails types reports its REAL error, never the shape diagnostic.
void ExprResolveAccessor::ApplyRescueStash(SnExpressionList &exprs)
{
	for (size_t i = 0; i < m_rescueStash.size(); )
	{
		if (m_rescueStash[i].pList != &exprs)
		{
			++i;
			continue;
		}
		RescueStash stash = std::move(m_rescueStash[i]);
		m_rescueStash.erase(m_rescueStash.begin() + i);

		auto *pOwner = stash.pMisread->Parent();
		NLANG_ASSERT(pOwner);
		auto iPos = pOwner->Children().find(stash.pMisread);
		NLANG_ASSERT(iPos != pOwner->Children().end());
		//RemoveChildFrom returns the FOLLOWING iterator; erase invalidates
		//the erased one, so the first insert must start from the return.
		iPos = RemoveChildFrom(iPos, *pOwner);
		DeleteNode(stash.pMisread);
		for (auto *pRescued : stash.rescued)
		{
			iPos = InsertChildInto(iPos, pRescued, *pOwner);
			++iPos;
		}
		for (auto *pRescued : stash.rescued)
		{
			if (!pRescued->IsResolved())
				pRescued->Accept(*m_pVisitor);
		}
	}
}

bool ExprResolver::ResolveDataTypes(SnField &sn, SnField &outerType)
{
	if (sn.IsDataField())
	{
		if (sn.IsResolved())
			return true;

		if (sn.Kind() != NK_Function)
		{
			//Mixed kinds reach here (struct/class fields, formal
			//params); the field classes are siblings that each keep
			//their own type slot, so the type expression comes through
			//the virtual, never a static_cast between the siblings.
			SnFieldExpr *pDeclaredType = sn.DeclaredTypeExpr();
			NLANG_ASSERT(pDeclaredType);
			if (!ResolveDataType(*pDeclaredType, outerType))
				return false;
			//Array redesign B: jagged declarations (T[][]) have no VM
			//layout and used to degrade silently — reject here.
			if (ArrayTypeDepth(pDeclaredType) >= 2)
				m_Accessor.m_Env.Log(CLL_Error, sn.Location(),
					"jagged arrays (T[][]) are not supported");
			return true;
		}

		auto pReturnType = static_cast<SnFunction &>(sn).ReturnType();
		if (pReturnType)
		{
			if (!ResolveDataType(*pReturnType, outerType))
				return false;
			//Array redesign B: jagged return types are rejected like the
			//other declaration forms (no VM layout).
			if (ArrayTypeDepth(
				static_cast<SnFunction &>(sn).ReturnType()) >= 2)
				m_Accessor.m_Env.Log(CLL_Error, sn.Location(),
					"jagged arrays (T[][]) are not supported");
		}
	}

	if (!ResolveChildFields(sn))
		return false;
	sn.AddFlags(NF_Resolved);
	return true;
}

bool ExprResolver::ResolveDataType(SnFieldExpr &typeExpr, SnField &outerType)
{
	if (!Resolve(typeExpr, outerType, outerType, ERF_None))
	{
		typeExpr.AddFlags(NF_Invalid);
		return false;
	}
	//v0.12.2: declaration-side type positions (fields, formals,
	//returns) reject the prelude face classes — they declare method
	//faces, not usable types.
	RejectPreludeFaceUse(m_Accessor.m_Env, typeExpr.Field(),
		typeExpr.Location(), "used as a type");
	return true;
}

bool ExprResolver::ResolveChildFields(SnField & sn)
{
	bool bOK = true;
	for (auto &child : sn.Children())
	{
		if (child.IsField())
		{
			auto &childField = static_cast<SnField &>(child);
			if (!ResolveDataTypes(childField, sn) && bOK)
				bOK = false;
		}
	}
	return bOK;
}

} //namespace nlang
