/*---
    ExprResolverInvokeMatch.cpp — 调用候选收集与重载匹配
    从 ExprResolverInvoke.cpp 抽取（2026-10-12 source-size 拆分，零行为变化）；
    接收者分类家族移居 ExprResolverInvokeReceiver.cpp（2026-10-30）。
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

namespace
{

bool MatchesReceiverForm(const SnFunction &func, InvokeReceiverForm form)
{
	if (form == InvokeReceiverForm::TypeName)
		return func.ContainFlags(NF_Static);
	if (form == InvokeReceiverForm::Value)
		return !func.ContainFlags(NF_Static);
	return true;
}

//0.9.12 D-new-6: a non-static member named after its class is that
//class's ctor (T1.5c makes every other same-name shape illegal). It
//stays out of the bare-invoke pool — a bare binding hides the ctor
//fallback (TryResolveCtorInvoke); other forms/statics keep their paths.
bool IsConstructorMember(const SnFunction &func, const SnField &parent)
{
	return parent.Kind() == NK_ClassDecl
		&& !func.ContainFlags(NF_Static)
		&& func.Name() == static_cast<const SnClassDecl &>(parent).Name();
}

} //namespace

//Search a single scope's NameDict for matching functions. Bare-pool
//scopes (root / namespaces) additionally drop foreign owned functions
//(D1/D7) — skipped entirely, so they never set bImportedMatch either.
//Candidates are filtered by call form first: statics and instance
//members live in separate namespaces (0.9.10 D2.4).
void ExprResolveAccessor::SearchFuncScope(SnFunctionParentField &parent,
	bool bBarePool, const std::string &sFuncName, uint32_t curModule,
	InvokeReceiverForm form, std::vector<SnFunction*> &candidates,
	bool &rbImportedMatch)
{
	auto range = parent.Members().NameDict().equal_range(sFuncName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField *pField = iField->second;
		if (pField->Kind() != NK_Function)
			continue;

		auto pFunc = static_cast<SnFunction *>(pField);
		if (!MatchesReceiverForm(*pFunc, form))
			continue;
		if (form == InvokeReceiverForm::Bare && IsConstructorMember(*pFunc, parent))
			continue;
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
	const std::string &sFuncName, InvokeReceiverForm form,
	std::vector<SnFunction*> &candidates, bool &rbImportedMatch)
{
	auto range = enumDecl.Methods().NameDict().equal_range(sFuncName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		auto *pFunc = static_cast<SnFunction *>(iField->second);
		if (!MatchesReceiverForm(*pFunc, form))
			continue;
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
	InvokeReceiverForm form, std::vector<SnFunction*> &candidates,
	bool &rbImportedMatch)
{
	auto *pSuper = classDecl.SuperClass();
	while (pSuper && candidates.empty())
	{
		SearchFuncScope(*pSuper, false, sFuncName, curModule, form,
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
//
//D1/D7: the bare pool only spans the current TU's directory; the
//candidate filter below is fed with the context owner computed once.
//This is the BARE-POOL owner filter (ShareBarePool), not the import
//gate — the gate policy lives in ModuleRegistry::ImportGateAllows.
//0.9.10 D2.4: the receiver form selects which method namespace this
//invoke may bind to (statics vs instance members).
bool ExprResolveAccessor::CollectInvokeCandidates(SnInvokeExpr &invoke,
	bool bSearchInAncestor, std::vector<SnFunction*> &candidates,
	bool &rbImportedMatch, bool &rbVisibilityHintLogged)
{
	auto &sFuncName = invoke.CalleeName();
	const uint32_t curModule = m_Env.Registry().OwnerOfContext(*m_pContext);
	const InvokeReceiverForm form = ClassifyInvokeReceiver(invoke);

	SyntaxNode *pParent = m_pContext;
	while (pParent)
	{
		if (CanBeFuncParentEx(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField*>(pParent);
			//Root and namespaces are the bare pool (IsBarePoolScope); class
			//and interface scopes keep full visibility (D1/D7).
			SearchFuncScope(*pParentType, IsBarePoolScope(*pParent),
				sFuncName, curModule, form, candidates, rbImportedMatch);
			if (pParent->Kind() == NK_ClassDecl && candidates.empty())
				SearchSuperclassChain(
					*static_cast<SnClassDecl*>(pParent), sFuncName,
					curModule, form, candidates, rbImportedMatch);
			if (!bSearchInAncestor)
				break;
		}
		else if (pParent->Kind() == NK_EnumDecl)
		{
			SearchEnumScope(static_cast<SnEnumDecl&>(*pParent), sFuncName,
				form, candidates, rbImportedMatch);
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

//M3b: the TryBindArgs / type-distance core of FindFuncByInvoke — the
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

	std::vector<FormalBinding> bestBindings;
	SnFunction *pBest = nullptr;
	SnFunction *pTied = nullptr;
	const int nBest = SweepInvokeCandidates(invoke, candidates,
		bestBindings, rbAmbiguous, pBest, pTied);
	if (nBest < 0)
		return FFR_Incompatible;
	if (rbAmbiguous)
	{
		LogAmbiguousCall(invoke, pBest, pTied);
		return FFR_Incompatible;
	}
	pFunc = pBest;
	outBindings = std::move(bestBindings);
	return (nBest == 0) ? FFR_ExactMatch : FFR_ApproximateMatch;
}

//0.9.11 T6: the resolved actual-pointer list of an invoke — built once
//for the whole candidate sweep.
static std::vector<SnExpression*> CollectInvokeActuals(SnInvokeExpr &invoke)
{
	std::vector<SnExpression*> actuals;
	for (auto &param : invoke.Params())
		actuals.push_back(&param);
	return actuals;
}

//FindFuncByInvoke's bare-path head: candidate collection plus the shared
//matching core. Contract (header decl): the out-params are always
//initialized — an uninitialized caller local holding stack garbage once
//crashed Access(SnInvokeExpr) via `if (pCallee)`.
FindFuncResult ExprResolveAccessor::FindFuncByInvoke(SnFunction *&pFuncFound,
	SnInvokeExpr &invoke, std::vector<FormalBinding> &outBindings,
	bool &rbNameMatchedImported, bool &rbVisibilityHintLogged,
	std::vector<SnFunction*> *pOutCandidates, bool *pbAmbiguousCall)
{
	pFuncFound = nullptr;
	rbNameMatchedImported = false;
	rbVisibilityHintLogged = false;
	if (pbAmbiguousCall)
		*pbAmbiguousCall = false;
	bool bImportedMatch = false;
	std::vector<SnFunction*> candidates;
	if (!CollectInvokeCandidates(invoke,
			!ContainFlags(ERF_SearchInParentOnly), candidates,
			bImportedMatch, rbVisibilityHintLogged))
		return FFR_FuncNameNotFound;
	if (pOutCandidates)
		*pOutCandidates = candidates;

	//One matching core for both call paths; the ambiguity log lives
	//there. The ambiguity flag rides out so the caller's generic failure
	//text stays silent after the named report (v0.12.7); on a
	//no-viable-bind Incompatible, rbNameMatchedImported surfaces whether
	//an imported stub was among the name matches (its parameter types
	//are synthesized — the verdict may just mean the real signature was
	//not visible).
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pFuncFound,
		outBindings, bAmbiguous);
	if (pbAmbiguousCall)
		*pbAmbiguousCall = bAmbiguous;
	if (res == FFR_Incompatible && !bAmbiguous)
	{
		pFuncFound = nullptr;
		rbNameMatchedImported = bImportedMatch;
	}
	return res;
}

//The candidate sweep under MatchInvokeAgainst: bind every candidate,
//keep the strictest viable distance, and break equal-distance ties
//toward this TU's OWN definitions (C# namespace-member / Rust
//local-beats-glob precedent: a local same-shape declaration wins
//silently; only import-vs-import ties stay the loud use-site
//ambiguity). The tied pair is retained for the named diagnostic
//(v0.12.7 V4). Returns the best distance; -1 when nothing binds.
int ExprResolveAccessor::SweepInvokeCandidates(SnInvokeExpr &invoke,
	const std::vector<SnFunction*> &candidates,
	std::vector<FormalBinding> &outBestBindings, bool &rbAmbiguous,
	SnFunction *&rpBest, SnFunction *&rpTied)
{
	const std::vector<SnExpression*> actuals = CollectInvokeActuals(invoke);
	auto &reg = m_Env.Registry();
	const uint32_t curModule = reg.OwnerOfContext(*m_pContext);
	auto isOwnTu = [&reg, curModule](SnFunction *pFunc) {
		return reg.OwnerOf(*pFunc) == curModule;
	};
	int nBestDistance = -1;
	rpBest = nullptr;
	rpTied = nullptr;
	for (auto *pCandidate : candidates)
	{
		std::vector<FormalBinding> tryBind;
		if (!TryBindArgs(actuals, *pCandidate, tryBind))
			continue;
		const int n = ComputeBindingDistance(tryBind);
		if (n < 0)
			continue;
		if (nBestDistance < 0 || n < nBestDistance)
		{
			nBestDistance = n;
			rpBest = pCandidate;
			outBestBindings = std::move(tryBind);
			rbAmbiguous = false;
		}
		else if (n == nBestDistance)
		{
			if (isOwnTu(pCandidate) == isOwnTu(rpBest))
			{
				rbAmbiguous = true;
				rpTied = pCandidate;
			}
			else if (isOwnTu(pCandidate))
			{
				//The own-TU candidate shadows the imported tie.
				rpBest = pCandidate;
				outBestBindings = std::move(tryBind);
				rbAmbiguous = false;
			}
			//An imported candidate loses to the own-TU best: skip.
		}
	}
	return nBestDistance;
}

//0.9.11 T6 (D10): the constructor-call twin of MatchInvokeAgainst — the
//same bind-then-closest-distance core over the candidate ctors of one
//class/struct. Fully silent: the caller owns the class/struct-flavored
//diagnostics, so no bindings are handed back (codegen rebuilds them
//from the selected ctor's Params()). rbAmbiguous separates a tie at the
//smallest viable distance from a plain no-viable-bind; both answer null.
SnFunction *ExprResolveAccessor::MatchCtorAgainst(
	const std::vector<SnExpression*> &actuals,
	const std::vector<SnFunction*> &candidates, bool &rbAmbiguous)
{
	rbAmbiguous = false;
	if (candidates.empty())
		return nullptr;

	int nBestDistance = -1;
	SnFunction *pBest = nullptr;

	for (auto *pCandidate : candidates)
	{
		std::vector<FormalBinding> tryBind;
		if (!TryBindArgs(actuals, *pCandidate, tryBind))
			continue;
		int n = ComputeBindingDistance(tryBind);
		if (n < 0)
			continue;
		if (nBestDistance < 0 || n < nBestDistance)
		{
			nBestDistance = n;
			pBest = pCandidate;
			rbAmbiguous = false;
		}
		else if (n == nBestDistance)
		{
			rbAmbiguous = true;
		}
	}
	if (nBestDistance < 0 || rbAmbiguous)
		return nullptr;
	return pBest;
}

} //namespace nlang
