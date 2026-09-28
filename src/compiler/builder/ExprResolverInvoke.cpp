/*---
    ExprResolverInvoke.cpp — 调用解析（候选收集、重载匹配、成功尾、失败诊断）
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

//Phase 13: delegate call — the callee name resolves to a Func-typed
//value (local / param / class field) rather than a function. Name
//lookup order puts the Func value first (Python-style shadowing of a
//same-named function); every path inside consumes the invoke.
bool ExprResolveAccessor::TryBindDelegateCall(SnInvokeExpr &snInvoke)
{
	if (auto *pDelegateField = FindDelegateTarget(snInvoke))
	{
		BindDelegateInvoke(snInvoke, pDelegateField);
		return true;
	}
	return false;
}

//Phase 12 (D9): a bare (receiver-less) invoke that binds to a METHOD
//is a frame-shift bug — codegen's bare-invoke path emits OP_CallFunc
//with slotBase 0 while the callee's frame expects `this` at slot 0,
//so the first argument is read as the receiver (enum methods return
//silent garbage; class methods fail with a field-access error — the
//class side pre-dates Phase 12). The grammar only produces method
//calls as the Inner of a MemberExpr (`c.f()`, `this.f()`); an invoke
//in any other position (statement, nested argument, outer of a member
//access) that binds to a method is bare. Free functions (parent
//namespace) are unaffected. True = rejected (diagnostic logged).
bool ExprResolveAccessor::RejectBareMethodCall(SnInvokeExpr &snInvoke,
	SnFunction *pCallee)
{
	if (!pCallee)
		return false;
	auto *pInvokeParent = snInvoke.Parent();
	bool bIsMethodCallShape = pInvokeParent
		&& pInvokeParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr*>(pInvokeParent)->Inner() == &snInvoke;
	if (bIsMethodCallShape)
		return false;
	auto *pCalleeParent = pCallee->Parent();
	if (pCalleeParent && (pCalleeParent->Kind() == NK_ClassDecl
		|| pCalleeParent->Kind() == NK_InterfaceDecl
		|| pCalleeParent->Kind() == NK_EnumDecl))
	{
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"method \"%s\" must be called through a receiver "
			"(e.g. this.%s(...)).",
			pCallee->Name().c_str(), pCallee->Name().c_str());
		return true;
	}
	return false;
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	assert(!snInvoke.IsResolved());

	if (!ResolveExpressionList(snInvoke.Params()))
		return;

	//Phase 9c: caller-side syntax validation — independent of candidates.
	//Reports specific errors for structural issues that no overload can
	//satisfy (e.g. positional arg after named, duplicate named names).
	if (!ValidateInvokeSyntax(snInvoke))
		return;

	if (TryBindDelegateCall(snInvoke))
		return;

	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	std::vector<SnFunction*> candidates;
	bool bNameMatchedImported = false;
	bool bVisibilityHintLogged = false;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings,
		bNameMatchedImported, bVisibilityHintLogged, &candidates);

	//Phase 9e: out arguments on virtual (by-name dispatched) methods are
	//rejected before anything binds — see OutArgOnDispatchedCalleeRejected.
	if (pCallee
		&& OutArgOnDispatchedCalleeRejected(snInvoke, *pCallee, bindings))
		return;

	if (RejectBareMethodCall(snInvoke, pCallee))
		return;

	if (res == FFR_ExactMatch || res == FFR_ApproximateMatch)
	{
		//M3b: the success tail is shared with the module-qualified call
		//path. A false return means a function-reference argument failed
		//to bind (diagnostic already logged) — leave the invoke unresolved.
		(void)ResolveInvokeWithFunc(snInvoke, *pCallee, res, bindings);
		return;
	}

	//Module import visibility (F20/M4): the "not visible here" hint is
	//the complete diagnosis — appending the generic failure text would
	//only blur it.
	if (!bVisibilityHintLogged)
		LogInvokeFailure(snInvoke, res, pCallee, bNameMatchedImported,
			candidates);
}

//Phase 9c: validate caller-side argument syntax (candidate-independent).
//Reports specific errors for:
//  - positional arg following a named arg
//  - duplicate name in named args (e.g. foo(a=1, a=2))
//Returns true if well-formed; false after logging.
bool ExprResolveAccessor::ValidateInvokeSyntax(const SnInvokeExpr &invoke)
{
	bool bSeenNamed = false;
	std::set<std::string> seenNames;
	for (auto &actual : invoke.Params())
	{
		if (actual.Kind() != NK_NamedArgExpr)
		{
			if (bSeenNamed)
			{
				m_Env.Log(CLL_Error, actual.Location(),
					"positional argument cannot follow a named argument in "
					"call to \"%s\".",
					invoke.CalleeName().c_str());
				return false;
			}
			continue;
		}
		auto &named = static_cast<const SnNamedArgExpr&>(actual);
		bSeenNamed = true;
		auto [it, inserted] = seenNames.emplace(named.Name());
		if (!inserted)
		{
			m_Env.Log(CLL_Error, named.Location(),
				"duplicate named argument \"%s\" in call to \"%s\".",
				named.Name().c_str(), invoke.CalleeName().c_str());
			return false;
		}
	}
	return true;
}

//Search a single scope's NameDict for matching functions. Bare-pool
//scopes (root / namespaces) additionally drop foreign owned functions
//(D1/D7) — skipped entirely, so they never set bImportedMatch either.
void ExprResolveAccessor::SearchFuncScope(SnFunctionParentField &parent,
	bool bBarePool, const std::string &sFuncName, uint32_t curModule,
	std::vector<SnFunction*> &candidates, bool &rbImportedMatch)
{
	auto range = parent.Members().NameDict().equal_range(sFuncName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField *pField = iField->second;
		if (pField->Kind() != NK_Function)
			continue;

		auto pFunc = static_cast<SnFunction *>(pField);
		if (!pFunc->AllowAccess(*m_pAccessor))
			continue;
		if (bBarePool && !IsBareVisible(*pFunc, curModule))
			continue;

		if (pFunc->ContainFlags(NF_Imported))
			rbImportedMatch = true;
		candidates.push_back(pFunc);
	}
}

//Phase 12: enum method scan. SnEnumDecl is not a SnFunctionParentField —
//its members and methods are separate kind-filtered child lists — so
//SearchFuncScope cannot be reused. Enums have no inheritance chain and
//no bare-pool filtering (methods are owned by the enum itself).
void ExprResolveAccessor::SearchEnumScope(SnEnumDecl &enumDecl,
	const std::string &sFuncName, std::vector<SnFunction*> &candidates,
	bool &rbImportedMatch)
{
	auto range = enumDecl.Methods().NameDict().equal_range(sFuncName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		auto *pFunc = static_cast<SnFunction *>(iField->second);
		if (!pFunc->AllowAccess(*m_pAccessor))
			continue;
		if (pFunc->ContainFlags(NF_Imported))
			rbImportedMatch = true;
		candidates.push_back(pFunc);
	}
}

//For class contexts, also search the inheritance chain when the method
//is not found in the current class's Members(). A superclass is a class
//scope, never a bare-pool scope.
void ExprResolveAccessor::SearchSuperclassChain(SnClassDecl &classDecl,
	const std::string &sFuncName, uint32_t curModule,
	std::vector<SnFunction*> &candidates, bool &rbImportedMatch)
{
	auto *pSuper = classDecl.SuperClass();
	while (pSuper && candidates.empty())
	{
		SearchFuncScope(*pSuper, false, sFuncName, curModule,
			candidates, rbImportedMatch);
		pSuper = pSuper->SuperClass();
	}
}

//Phase 9c candidate collection for the bare invoke path: walk the scope
//chain from the resolution context, searching every function-parent and
//enum scope for accessible same-name functions. A member-call context
//(ERF_SearchInParentOnly) stops at its own scope — method-call syntax
//does not fall through to namespace scope. Returns false when the pool
//came up empty, with the F20 visibility hint already adjudicated
//(rbVisibilityHintLogged set when it fired).
bool ExprResolveAccessor::CollectInvokeCandidates(SnInvokeExpr &invoke,
	bool bSearchInAncestor, std::vector<SnFunction*> &candidates,
	bool &rbImportedMatch, bool &rbVisibilityHintLogged)
{
	auto &sFuncName = invoke.CalleeName();
	//D1/D7: the bare pool only spans the current TU's directory; the
	//candidate filter below is fed with the context owner computed once.
	const uint32_t curModule = m_Env.Registry().OwnerOfContext(*m_pContext);

	SyntaxNode *pParent = m_pContext;
	while (pParent)
	{
		if (CanBeFuncParentEx(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField*>(pParent);
			//Root and namespaces are the bare pool (IsBarePoolScope); class
			//and interface scopes keep full visibility (D1/D7).
			SearchFuncScope(*pParentType, IsBarePoolScope(*pParent),
				sFuncName, curModule, candidates, rbImportedMatch);
			if (pParent->Kind() == NK_ClassDecl && candidates.empty())
				SearchSuperclassChain(
					*static_cast<SnClassDecl*>(pParent), sFuncName,
					curModule, candidates, rbImportedMatch);
			if (!bSearchInAncestor)
				break;
		}
		else if (pParent->Kind() == NK_EnumDecl)
		{
			SearchEnumScope(static_cast<SnEnumDecl&>(*pParent), sFuncName,
				candidates, rbImportedMatch);
			if (!bSearchInAncestor)
				break;
		}
		pParent = pParent->Parent();
	}

	//F20: nothing on the (narrowed) bare pool carries the name. When the
	//name does exist elsewhere on the pool's surface, the visibility hint
	//is the real diagnosis — rbVisibilityHintLogged tells the caller to
	//skip its generic text (M4). The verdict stays FuncNameNotFound: the
	//name IS unknown to this pool, no type mismatch happened.
	if (candidates.empty()
		&& MaybeLogVisibilityHint(sFuncName, invoke.Location(), curModule))
		rbVisibilityHintLogged = true;
	return !candidates.empty();
}

FindFuncResult ExprResolveAccessor::FindFuncByInvoke(SnFunction *&pFuncFound,
	SnInvokeExpr &invoke, std::vector<FormalBinding> &outBindings,
	bool &rbNameMatchedImported, bool &rbVisibilityHintLogged,
	std::vector<SnFunction*> *pOutCandidates)
{
	//Contract: the out-params are always initialized. The NotFound path
	//returns early without touching pFuncFound — an uninitialized caller
	//local then holds stack garbage, and `if (pCallee)` in
	//Access(SnInvokeExpr) dereferences a dangling pointer (ncc crash;
	//observed when imported stubs shifted stack layout). Clearing here
	//covers every path.
	pFuncFound = nullptr;
	rbNameMatchedImported = false;
	rbVisibilityHintLogged = false;
	const bool bSearchInAncestor = !ContainFlags(ERF_SearchInParentOnly);
	bool bImportedMatch = false;

	//Collect the same-name accessible candidates along the scope chain;
	//the bind/distance core itself is shared with the module-qualified
	//call path via MatchInvokeAgainst below.
	std::vector<SnFunction*> candidates;
	if (!CollectInvokeCandidates(invoke, bSearchInAncestor, candidates,
			bImportedMatch, rbVisibilityHintLogged))
		return FFR_FuncNameNotFound;
	//Hand the name-matched set to the caller so the failure path can log
	//per-argument detail (the set is non-empty here).
	if (pOutCandidates)
		*pOutCandidates = candidates;

	//One matching core for both call paths; the ambiguity log lives there.
	//rbNameMatchedImported stays reserved for the no-viable-bind verdict
	//(the ambiguity report is complete on its own) — the distinction is
	//observable through the imported-argument diagnostic.
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pFuncFound,
		outBindings, bAmbiguous);
	if (res == FFR_Incompatible && !bAmbiguous)
	{
		//At least one candidate matched by name but none could bind.
		//Surface whether an imported stub was among them — its parameter
		//types are synthesized, so the Incompatible verdict may simply
		//mean the compiler could not see the real signature.
		pFuncFound = nullptr;
		rbNameMatchedImported = bImportedMatch;
	}
	return res;
}

//M3b: the TryBindInvoke / type-distance core of FindFuncByInvoke — the
//single matching implementation shared by the bare path (candidates
//collected along the scope chain there) and the module-qualified path
//(candidates from the module table). Silent on not-found / incompatible
//— the caller reports those with its own context; the ambiguity error
//is logged HERE (candidate-set independent) and reported through
//rbAmbiguous so callers can tell it apart from a no-viable-bind
//Incompatible. pFunc is nulled on every failure path.
FindFuncResult ExprResolveAccessor::MatchInvokeAgainst(SnInvokeExpr &invoke,
	const std::vector<SnFunction*> &candidates, SnFunction *&pFunc,
	std::vector<FormalBinding> &outBindings, bool &rbAmbiguous)
{
	pFunc = nullptr;
	rbAmbiguous = false;
	if (candidates.empty())
		return FFR_FuncNameNotFound;

	int nBestDistance = -1;
	SnFunction *pBest = nullptr;
	std::vector<FormalBinding> bestBindings;

	//Bind, then keep the strictest viable distance; a tie at the
	//smallest viable distance is ambiguous.
	auto consider = [&](SnFunction *pCandidate) {
		std::vector<FormalBinding> tryBind;
		if (!TryBindInvoke(invoke, *pCandidate, tryBind))
			return;
		int n = ComputeBindingDistance(tryBind);
		if (n < 0)
			return;
		if (nBestDistance < 0 || n < nBestDistance)
		{
			nBestDistance = n;
			pBest = pCandidate;
			bestBindings = std::move(tryBind);
			rbAmbiguous = false;
		}
		else if (n == nBestDistance)
		{
			rbAmbiguous = true;
		}
	};

	for (auto *pCandidate : candidates)
		consider(pCandidate);

	if (nBestDistance < 0)
		return FFR_Incompatible;
	if (rbAmbiguous)
	{
		LogAmbiguousCall(invoke);
		return FFR_Incompatible;
	}
	pFunc = pBest;
	outBindings = std::move(bestBindings);
	return (nBestDistance == 0) ? FFR_ExactMatch : FFR_ApproximateMatch;
}

//M3b: the SUCCESS tail of Access(SnInvokeExpr), shared with the
//module-qualified call path so both bind a callee exactly alike.
bool ExprResolveAccessor::ResolveInvokeWithFunc(SnInvokeExpr &invoke,
	SnFunction &func, FindFuncResult match,
	std::vector<FormalBinding> &bindings)
{
	//Phase 13: argument-position function references bind against the
	//formal Func types of the chosen overload (ComputeBindingDistance
	//already required an exact signature match for candidacy). Step 2
	//adds the receiver-bound member form (c.foo).
	if (match == FFR_ExactMatch || match == FFR_ApproximateMatch)
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
					b.pFormal->EvalDataType()))
				return false;
		}
	}

	if (match == FFR_ApproximateMatch)
		FixupParamTypesWithBindings(invoke, bindings);
	invoke.SetBindings(std::move(bindings));
	//A void return resolves to a null EvalDataType here — the established
	//void-invoke convention.
	ResolveFieldExprAs(invoke, &func);
	BindArrayTypeToken(invoke);
	return true;
}

//Phase 9e: out arguments on virtual methods are rejected — the
//writeback mask is baked into the call instruction against the
//static callee's parameter layout; a runtime override resolved by
//name-based dispatch could disagree with it. Interface methods are
//always dispatched by name (never NF_Virtual-flagged), so they are
//covered via the parent decl kind. Shared by the bare and the
//module-qualified call paths.
bool ExprResolveAccessor::OutArgOnDispatchedCalleeRejected(
	const SnInvokeExpr &invoke, const SnFunction &callee,
	const std::vector<FormalBinding> &bindings) const
{
	bool bDispatchedByName = callee.ContainFlags(NF_Virtual)
		|| (callee.Parent()
			&& callee.Parent()->Kind() == NK_InterfaceDecl);
	if (!bDispatchedByName)
		return false;
	for (auto &b : bindings)
	{
		if (b.bIsOut)
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"out arguments are not supported on virtual method "
				"\"%s\".",
				callee.Name().c_str());
			return true;
		}
	}
	return false;
}

//The failure diagnostics of the invoke paths live in
//ExprResolverInvokeDiag.cpp (LogInvokeFailure and its MaybeLog* sweeps).

} //namespace nlang
