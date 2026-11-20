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
//NLANG_ASSERT (diagnostic assert with stack dump) for the invariant below.
#include <nlang/runtime/StackDump.h>

namespace nlang
{

void ExprResolveAccessor::ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
{
	NLANG_ASSERT(pField != nullptr);
	//Idempotent re-binding (the resolution passes' stated contract, at
	//this chokepoint): the declared-generic creation's completion window can
	//resolve a member type expression while the outer attempt that
	//TRIGGERED the creation is still on its own binding path — a fixed-key
	//self-reference (`List<T> copy()` inside `List<T>`) hands the same
	//node to this function twice, and the second visit carries the SAME
	//two-phase cached instance. A consistent re-binding is a no-op; only
	//a CONFLICTING one (different target) breaks the invariant.
	if (expr.IsResolved())
	{
		NLANG_ASSERT(expr.Field() == pField);
		return;
	}

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
	std::function<bool(SnField &)> pFuncFilter,
	const ISourceLocation *pSpillLoc)
{
	SnField *pField = parent.FindField(sName);
	if (pField && AccessAdmitted(m_Env.Registry(), *pField, accessor))
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
		//A filter-rejected candidate reads as "not found": without SPO the
		//fall-through below keeps walking ancestors (a same-name function
		//higher up may still be visible); with SPO the single miss return
		//below reports the rejection. (The filter and SPO flag sets never
		//co-occur today — the sole filter call site Access(SnIdentifierExpr)
		//never runs with SPO — re-verify when call-site configs change.)
	}
	//v0.12.6: the scope's child enums answer after its own members miss
	//(the unscoped-enum spill, FindEnumMemberSpill below). Runs on every
	//miss path — a found-but-inaccessible or filter-rejected direct
	//member is a miss here too, matching the doctrine above.
	if (SnField *pSpill = FindEnumMemberSpill(parent, sName, accessor,
			pSpillLoc))
		return pSpill;
	//A found-but-inaccessible field is a miss too: under SPO (member
	//access, using-directive lookup) the search stops here and the caller
	//diagnoses "cannot resolve"; without SPO an ancestor scope with a
	//visible same-named member may still answer.
	if (flags & ERF_SearchInParentOnly)
		return nullptr;
	auto pParent = parent.Parent();
	if (!pParent)
		return nullptr;
	return FindFieldInAncestor(sName, *pParent, accessor, flags, pFuncFilter,
		pSpillLoc);
}

SnField *ExprResolveAccessor::FindEnumMemberSpill(SyntaxNode &scope,
	const std::string &sName, const SnField &accessor,
	const ISourceLocation *pLoc)
{
	SnEnumMember *pHit = nullptr;
	SnEnumDecl *pHitOwner = nullptr;
	for (auto &node : scope.Children())
	{
		if (node.Kind() != NK_EnumDecl)
			continue;
		auto &decl = static_cast<SnEnumDecl &>(node);
		if (decl.IsImported())
			continue;
		auto *pMember = static_cast<SnEnumMember *>(decl.FindField(sName));
		if (!pMember || !AccessAdmitted(m_Env.Registry(), *pMember, accessor))
			continue;
		if (pHit)
		{
			//Same scope, two enums, one member name: answering with either
			//would bind a possibly wrong constant silently. The logged
			//error fails the build, so the first hit is returned.
			m_Env.Log(CLL_Error, pLoc,
				"The name \"%s\" is ambiguous: it matches members of the "
				"enums \"%s\" and \"%s\". Qualify the member with its "
				"enum name.",
				sName.c_str(), pHitOwner->Name().c_str(),
				decl.Name().c_str());
			return pHit;
		}
		pHit = pMember;
		pHitOwner = &decl;
	}
	return pHit;
}

//0.9.x shadow guard (see ExprResolver.h).
bool IsTypeOrNamespaceBinding(const SnField *pField)
{
	return pField->IsTypeField() || pField->Kind() == NK_Namespace;
}

//0.13.8 D8.4 (Task 9): every same-named type candidate among the
//scope's OWN bare-pool members, in registration order. Child
//namespaces stay out on purpose: their members are reachable only in
//the qualified form, so they are not bare-position candidates at this
//scope (the function-side scan recurses because its miss hint names
//hidden providers anywhere in the pool surface — a different duty).
static void CollectSameNameTypes(SnFunctionParentField &pool,
	const std::string &sName, std::vector<SnField *> &rOut)
{
	auto range = pool.Members().NameDict().equal_range(sName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		if (iField->second->IsTypeField())
			rOut.push_back(iField->second);
	}
}

//0.13.8 D8.4 (Task 9): bare type resolution at one bare-pool scope.
//The same top-level type name may be declared by several units (the
//DuplicateFieldChecker exempts cross-pool same-name), and merged
//members hang flat under one scope, so the single-candidate FindField
//answer is the first-registered child regardless of origin — a later
//unit silently bound the earlier unit's declaration in every bare
//type position (annotations, ctor probes, casts, typeName operands).
//Selection order: the accessor's own unit wins outright; otherwise the
//function-side admission model (ownerless / ShareBarePool /
//IsExactImportedBy) filters — one admitted candidate answers, several
//diagnose the ambiguity naming every owner. A single never-admitted
//foreign candidate stays visible (types are global, spec §5.5). True =
//rpOut holds the answer; false = no answer at this scope, with
//rbAmbiguous marking the diagnosed ambiguity so the ancestor walk must
//not fall back to the first-registered child.
//0.13.8 D8.4 (Task 9): the pool admission half of the visibility model,
//shared with the function side (IsBareVisible above): ownerless
//built-ins, same-directory pools, and exact imports are admitted;
//everything else stays foreign.
static bool PoolAdmitted(const ModuleRegistry &registry, uint32_t owner,
	uint32_t curModule)
{
	if (owner == ModuleRegistry::NO_OWNER)
		return true;
	if (curModule == ModuleRegistry::NO_OWNER)
		return false;
	return registry.ShareBarePool(owner, curModule)
		|| registry.IsExactImportedBy(curModule, owner);
}

//0.13.8 D8.4 (Task 9): the cross-pool ambiguity diagnostic, naming
//every owner so the fix is actionable (the same wording discipline as
//the enum-member collision above).
static void LogPoolTypeAmbiguity(BuildEnvironment &env,
	const ModuleRegistry &registry, const std::string &sName,
	const SnField &accessor, const std::vector<SnField *> &candidates)
{
	std::string packages;
	for (auto *pCandidate : candidates)
	{
		if (!packages.empty())
			packages += ", ";
		packages += registry.OwnerOf(*pCandidate)
				== ModuleRegistry::NO_OWNER
			? "'<builtins>'"
			: "'" + registry.PackageOf(*pCandidate) + "'";
	}
	env.Log(CLL_Error, accessor.Location(),
		"The type \"%s\" is ambiguous: it matches declarations in "
		"packages %s. Qualify the name with its package, or rename "
		"one of the declarations.",
		sName.c_str(), packages.c_str());
}

//0.13.8 D8.4 (Task 9): scan the same-name candidates in registration
//order — the accessor's own unit wins outright (one unit declares a
//given name at most once, so the scan stops there); every other
//candidate goes through the admission filter. Setting the own-unit
//winner leaves the admitted list short on purpose: the caller's
//ambiguity branch is guarded by the winner, not by the list.
static void ScanPoolCandidates(const ModuleRegistry &registry,
	const std::vector<SnField *> &candidates, uint32_t curModule,
	SnField *&rpOwn, std::vector<SnField *> &rAdmitted)
{
	for (auto *pCandidate : candidates)
	{
		const uint32_t owner = registry.OwnerOf(*pCandidate);
		if (owner != ModuleRegistry::NO_OWNER && owner == curModule)
		{
			rpOwn = pCandidate;
			break;
		}
		if (PoolAdmitted(registry, owner, curModule))
			rAdmitted.push_back(pCandidate);
	}
}

bool ExprResolveAccessor::FindTypeInPoolScope(const std::string &sName,
	SyntaxNode &scope, const SnField &accessor, SnField *&rpOut,
	bool &rbAmbiguous)
{
	rpOut = nullptr;
	rbAmbiguous = false;
	auto &registry = m_Env.Registry();
	std::vector<SnField *> sameName;
	CollectSameNameTypes(static_cast<SnFunctionParentField &>(scope),
		sName, sameName);
	if (sameName.empty())
		return false;
	const uint32_t curModule = registry.OwnerOfContext(accessor);
	SnField *pOwn = nullptr;
	std::vector<SnField *> admitted;
	ScanPoolCandidates(registry, sameName, curModule, pOwn, admitted);
	//Several same-name candidates with no own-unit winner: diagnose the
	//ambiguity naming every owner instead of answering with the
	//first-registered child. (ScanPoolCandidates leaves the admitted
	//list short when the own-unit winner is set — the pOwn guard keeps
	//own-wins out of this branch.)
	if (!pOwn && (admitted.size() > 1
		|| (admitted.empty() && sameName.size() > 1)))
	{
		LogPoolTypeAmbiguity(m_Env, registry, sName, accessor,
			admitted.empty() ? sameName : admitted);
		rbAmbiguous = true;
		return false;
	}
	if (pOwn || admitted.size() == 1)
	{
		SnField *pTyped = pOwn ? pOwn : admitted.front();
		if (AccessAdmitted(registry, *pTyped, accessor))
		{
			rpOut = pTyped;
			return true;
		}
		//Denied (a private pick): read as a miss at this scope; the
		//ancestor walk continues exactly as the legacy path does.
		return false;
	}
	//A single foreign candidate stays visible (types are global),
	//behind the same access gate as before.
	if (AccessAdmitted(registry, *sameName.front(), accessor))
	{
		rpOut = sameName.front();
		return true;
	}
	return false;
}

SnField *ExprResolveAccessor::FindTypeNameInAncestor(const std::string &sName,
	SyntaxNode &parent, const SnField &accessor, bool bIncludeNamespaces)
{
	//0.13.8 D8.4: at a bare-pool scope the same type name may be
	//declared by several units; pool scopes resolve types unit-aware
	//(FindTypeInPoolScope above). Namespaces merge across TUs (unique
	//per name) and value-kind hits keep the legacy single-candidate
	//path, so every miss below falls through unchanged.
	if (IsBarePoolScope(parent))
	{
		SnField *pTyped = nullptr;
		bool bAmbiguous = false;
		if (FindTypeInPoolScope(sName, parent, accessor, pTyped,
				bAmbiguous))
			return pTyped;
		if (bAmbiguous)
			return nullptr;
	}
	//Per scope the virtual FindField returns the single same-named
	//candidate (the DuplicateFieldChecker rejects same-scope duplicates),
	//so skipping a value hit cannot hide a same-scope type.
	SnField *pField = parent.FindField(sName);
	if (pField && AccessAdmitted(m_Env.Registry(), *pField, accessor)
		&& (pField->IsTypeField()
			|| (bIncludeNamespaces && pField->Kind() == NK_Namespace)))
	{
		return pField;
	}
	auto pParent = parent.Parent();
	if (!pParent)
		return nullptr;
	return FindTypeNameInAncestor(sName, *pParent, accessor,
		bIncludeNamespaces);
}

//Module import visibility (D1/D7 + v0.12.7 V1): the owner-side half of
//the bare-pool rule (scope side: IsBarePoolScope above). Ownerless
//symbols (root built-ins, runtime tables) stay visible — the filter
//applies ONLY to owned NK_Function members of bare-pool scopes (M2: a
//namespace declared in another directory is just as foreign);
//class/interface/enum scopes are untouched (types stay global, spec
//§5.5). Cross-module admission is the EXACT import gate (explicit
//`import a.b;` admits bare use — same-directory auto-adds ride the same
//list), so a wildcard never floods the bare pool. The same-directory
//pair check stays for the flat-library coincidence (library TU gates do
//not auto-add each other).
bool ExprResolveAccessor::IsBareVisible(SnFunction &func, uint32_t curModule)
{
	auto &reg = m_Env.Registry();
	const uint32_t owner = reg.OwnerOf(func);
	if (owner == ModuleRegistry::NO_OWNER)
		return true;
	if (owner == curModule)
		return true;
	if (reg.IsExactImportedBy(curModule, owner))
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
		NLANG_ASSERT(pNamespace);
		auto pField = FindFieldInAncestor(sName, *pNamespace, accessor,
			ERF_SearchInParentOnly);
		if (pField)
			return pField;
	}
	return nullptr;
}

//Every same-name function the bare pool scan would hide: name-dict
//range order within a scope, then child namespaces in insertion order.
//v0.12.7 V4: the full provider set (was: an arbitrary first hit) — the
//collision hint must name every module so the escape is actionable.
static void CollectOwnedFunctionsElsewhere(SnFunctionParentField &scope,
	const std::string &name,
	const std::function<bool(SnFunction &)> &isVisible,
	std::vector<SnFunction*> &rOut)
{
	auto range = scope.Members().NameDict().equal_range(name);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		if (iField->second->Kind() != NK_Function)
			continue;
		auto *pFunc = static_cast<SnFunction *>(iField->second);
		if (!isVisible(*pFunc))
			rOut.push_back(pFunc);
	}
	for (auto &member : scope.Members())
	{
		if (!IsBarePoolScope(member))
			continue;
		CollectOwnedFunctionsElsewhere(
			static_cast<SnNamespace &>(member), name, isVisible, rOut);
	}
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
	NLANG_ASSERT(IsBarePoolScope(*pRoot));
	std::vector<SnFunction*> hidden;
	CollectOwnedFunctionsElsewhere(
		static_cast<SnNamespace &>(*pRoot), name,
		[this, curModule](SnFunction &func)
		{
			return IsBareVisible(func, curModule);
		}, hidden);
	if (hidden.empty())
		return false;
	//v0.12.7: name EVERY provider and give both fixes — an explicit
	//import admits bare use too (V1), and the qualified spelling always
	//works once the module is admissible.
	std::string modules;
	for (auto *pFunc : hidden)
	{
		if (!modules.empty())
			modules += ", ";
		modules += "'" + m_Env.Registry().ModulePathOf(
			m_Env.Registry().OwnerOf(*pFunc)) + "'";
	}
	const std::string firstModule = m_Env.Registry().ModulePathOf(
		m_Env.Registry().OwnerOf(*hidden.front()));
	m_Env.Log(CLL_Error, pLoc,
		hidden.size() == 1
			? "Function '%s' is not visible here. It lives in module "
			  "%s. Add an explicit import ('import %s;') or qualify "
			  "the call ('%s.%s')."
			: "Function '%s' is not visible here. It lives in modules: "
			  "%s. Add an explicit import ('import %s;') or qualify "
			  "the call ('%s.%s').",
		name.c_str(), modules.c_str(), firstModule.c_str(),
		firstModule.c_str(), name.c_str());
	return true;
}

} //namespace nlang
