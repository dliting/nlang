/*---
    ExprResolverMemberFields.cpp — 字段路径解析：ResolveFieldExprAs / 祖先与 using 查找 / 裸池可见性与诊断提示。
    从 ExprResolver.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
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

void ExprResolveAccessor::ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
{
	assert(pField && !expr.IsResolved());

	expr.m_pField = pField;
	if (!expr.PostResolveCheck(m_Env))
	{
		expr.AddFlags(NF_Invalid);
		return;
	}

	expr.EvalDataType(pField->EvalDataType());
	expr.AddFlags(NF_Resolved);
	return;
}

//Module import visibility (D1/D7): THE single authority for "this scope
//is part of the bare pool" — the merged tree root (null parent) and every
//namespace. Every consumer of the narrowing rule (FindFieldInAncestor's
//filter gate, FindFuncByInvoke's scope walk, FindOwnedFunctionElsewhere's
//scan surface) must ask THIS predicate, so Tasks 7/9 can evolve the rule
//in one place. (Same-source discipline as the EvalDataType dispatch-order
//bug family: several sites, one definition.)
bool IsBarePoolScope(const SyntaxNode &scope)
{
	return scope.Parent() == nullptr || scope.Kind() == NK_Namespace;
}

SnField *ExprResolveAccessor::FindFieldInAncestor(const std::string &sName,
	SyntaxNode &parent, const SnField &accessor, ExprResolveFlagSet flags,
	std::function<bool(SnField &)> pFuncFilter)
{
	SnField *pField = parent.FindField(sName);
	if (pField && pField->AllowAccess(accessor))
	{
		//Module import visibility (D1/D7): the caller may narrow the bare
		//pool — a function candidate sitting in a bare-pool scope
		//(IsBarePoolScope) must also pass the filter. Every other scope
		//(class members etc.) and every non-function field is untouched.
		const bool rejectedByFilter = pFuncFilter && IsBarePoolScope(parent)
			&& pField->Kind() == NK_Function
			&& !pFuncFilter(*pField);
		if (!rejectedByFilter)
			return pField;
		//Shotgun: with ERF_SearchInParentOnly the fall-through below would
		//return the filter-rejected candidate as a hit; a rejected
		//candidate must read as "not found". Currently unreachable — the
		//sole filter call site (Access(SnIdentifierExpr)) never runs with
		//SPO, and a namespace receiver resolves its context to
		//SnType::Instance() rather than the NS node (SnNamespace::
		//EvalDataType), so SPO contexts never land on a bare-pool scope
		//with the filter set. Re-verify when call-site configs change.
		//Without SPO the fall-through correctly keeps walking ancestors: a
		//same-name function higher up may still be visible.
		if (flags & ERF_SearchInParentOnly)
			return nullptr;
	}
	if (flags & ERF_SearchInParentOnly)
		return pField;
	auto pParent = parent.Parent();
	if (!pParent)
		return nullptr;
	return FindFieldInAncestor(sName, *pParent, accessor, flags, pFuncFilter);
}

//Module import visibility (D1/D7): the owner-side half of the bare-pool
//rule (scope side: IsBarePoolScope above). Ownerless symbols (root
//built-ins, runtime tables) stay visible — the filter applies ONLY to
//owned NK_Function members of bare-pool scopes (M2: a namespace declared
//in another directory is just as foreign); class/interface/enum scopes
//are untouched (types stay global, spec §5.5). The owned-pair core
//delegates to ModuleRegistry::ShareBarePool (single authority).
bool ExprResolveAccessor::IsBareVisible(SnFunction &func, uint32_t curModule)
{
	auto &reg = m_Env.Registry();
	const uint32_t owner = reg.OwnerOf(func);
	if (owner == ModuleRegistry::NO_OWNER)
		return true;
	if (owner == curModule)
		return true;
	//Defensive: an untagged context cannot share a directory with anyone.
	//Reachable only if a caller skips the OwnerOfContext precondition —
	//ModuleRegistry.h (DirectoryOf contract, ~:84) requires callers to
	//rule NO_OWNER out before directory comparison, since NO_OWNER maps
	//to the same "" directory as the root.
	if (curModule == ModuleRegistry::NO_OWNER)
		return false;
	return reg.ShareBarePool(owner, curModule);
}

SnField *ExprResolveAccessor::FindFieldInUsings(std::string &sName,
	const UsingList &usings, const SnField & accessor)
{
	for (auto pUsing : usings)
	{
		if (!pUsing->IsResolved())
			continue;
		auto pNamespace = pUsing->Namespace();
		assert(pNamespace);
		auto pField = FindFieldInAncestor(sName, *pNamespace, accessor,
			ERF_SearchInParentOnly);
		if (pField)
			return pField;
	}
	return nullptr;
}

//the first scan hit: name-dict range order within a scope, then child
//namespaces in insertion order — deterministic, but an arbitrary pick
//among equals (any of them fixes the diagnosis).
static SnFunction* FindOwnedFunctionElsewhere(SnFunctionParentField &scope,
	const std::string &name,
	const std::function<bool(SnFunction &)> &isVisible)
{
	auto range = scope.Members().NameDict().equal_range(name);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		if (iField->second->Kind() != NK_Function)
			continue;
		auto *pFunc = static_cast<SnFunction *>(iField->second);
		if (!isVisible(*pFunc))
			return pFunc;
	}
	for (auto &member : scope.Members())
	{
		if (!IsBarePoolScope(member))
			continue;
		if (auto *pForeign = FindOwnedFunctionElsewhere(
				static_cast<SnNamespace &>(member), name, isVisible))
			return pForeign;
	}
	return nullptr;
}

bool ExprResolveAccessor::MaybeLogVisibilityHint(const std::string &name,
	const ISourceLocation *pLoc, uint32_t curModule)
{
	//Method-call context: a same-named global function was never reachable
	//through member resolution, so suggesting it would mislead.
	if (ContainFlags(ERF_SearchInParentOnly))
		return false;

	//Walk up to the merged tree root, then scan the bare pool's surface.
	auto *pRoot = m_pContext;
	while (pRoot->Parent())
		pRoot = pRoot->Parent();
	assert(IsBarePoolScope(*pRoot));
	auto *pForeign = FindOwnedFunctionElsewhere(
		static_cast<SnNamespace &>(*pRoot), name,
		[this, curModule](SnFunction &func)
		{
			return IsBareVisible(func, curModule);
		});
	if (!pForeign)
		return false;
	m_Env.Log(CLL_Error, pLoc,
		"Function '%s' is not visible here. It lives in module '%s'; "
		"import it and qualify the call.",
		name.c_str(),
		m_Env.Registry().ModulePathOf(
			m_Env.Registry().OwnerOf(*pForeign)).c_str());
	return true;
}

} //namespace nlang
