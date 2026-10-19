/*---
    ExprResolverInvokeMatch.cpp — 调用候选收集与重载匹配
    从 ExprResolverInvoke.cpp 抽取（2026-10-12 source-size 拆分，零行为变化）。
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

//0.9.10 D2.4: the member expr whose inner identifier is idExpr (the
//receiver.name value shape), or null for every other position.
SnMemberExpr *AsMemberInner(SnIdentifierExpr &idExpr)
{
	auto *pParent = idExpr.Parent();
	if (pParent && pParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr*>(pParent)->Inner() == &idExpr)
		return static_cast<SnMemberExpr*>(pParent);
	return nullptr;
}

//A namespace qualifier is not a receiver — module-level function
//references keep their own channel, unfiltered.
bool OuterIsNamespaceQualifier(SnExpression *pOuter)
{
	if (pOuter && (pOuter->Kind() == NK_IdentifierExpr
		|| pOuter->Kind() == NK_MemberExpr))
	{
		auto *pOuterField = static_cast<SnFieldExpr*>(pOuter)->Field();
		return pOuterField && pOuterField->Kind() == NK_Namespace;
	}
	return false;
}

} //namespace

//0.9.10 D2.4: a receiver expression that names a type (class /
//interface / enum) — that form selects the static namespace.
//Supported spelling is a bare identifier (spec D2.1); a
//package-qualified type receiver (`lib.Util.f(...)`) does not resolve
//and stays a documented limitation.
bool ReceiverNamesType(SnExpression *pOuter)
{
	if (!pOuter)
		return false;
	if (pOuter->Kind() != NK_IdentifierExpr
		&& pOuter->Kind() != NK_MemberExpr)
		return false;
	auto *pField = static_cast<SnFieldExpr*>(pOuter)->Field();
	return pField && (pField->Kind() == NK_ClassDecl
		|| pField->Kind() == NK_InterfaceDecl
		|| pField->Kind() == NK_EnumDecl);
}

//0.9.10 D2.4: first same-name member of ONE namespace (static vs
//instance) along the receiver's chain — the rebind and cross-namespace
//diagnostic search. 0.9.10 D5: imported class stubs carry their
//exported public statics, so the walk continues through them; builtins
//still stop the walk (their members are intrinsic stubs, never user
//statics). The enum and interface arms scan their own members only.
SnFunction *FindMethodInNamespace(SyntaxNode *pContext,
	const std::string &name, bool bStatic)
{
	if (pContext && pContext->Kind() == NK_EnumDecl)
	{
		for (auto &method : static_cast<SnEnumDecl*>(pContext)->Methods())
			if (method.Name() == name
				&& method.ContainFlags(NF_Static) == bStatic)
				return &method;
		return nullptr;
	}
	if (pContext && pContext->Kind() == NK_InterfaceDecl)
	{
		for (auto &member : static_cast<SnInterfaceDecl*>(pContext)->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& member.ContainFlags(NF_Static) == bStatic)
				return static_cast<SnFunction*>(&member);
		return nullptr;
	}
	if (!(pContext && pContext->Kind() == NK_ClassDecl))
		return nullptr;
	std::set<SnClassDecl*> seen;
	for (auto *p = static_cast<SnClassDecl*>(pContext); p && !seen.count(p);
		p = p->SuperClass())
	{
		seen.insert(p);
		if (p->IsBuiltinClass())
			break;
		for (auto &member : p->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& member.ContainFlags(NF_Static) == bStatic)
				return static_cast<SnFunction*>(&member);
	}
	return nullptr;
}

//0.9.10 D2.4: type-name receiver, method in a value position — never a
//legal form: an instance method is the wrong namespace, and the static
//value form is unsupported this version (plan Task 8 Step 3, recorded
//in known-limitations). The static-namespace search picks the message
//(it owns the name for a type-name receiver). Always rejects.
bool ExprResolveAccessor::RejectTypeNameMethodValue(SnIdentifierExpr &idExpr)
{
	if (FindMethodInNamespace(m_pContext, idExpr.Name(), true))
		m_Env.Log(CLL_Error, idExpr.Location(),
			"cannot reference the static method \"%s\": static methods "
			"must be called directly.",
			idExpr.Name().c_str());
	else
		m_Env.Log(CLL_Error, idExpr.Location(),
			"the instance method \"%s\" must be called through a receiver "
			"object.",
			idExpr.Name().c_str());
	return false;
}

//0.9.10 D2.4: the member VALUE channel (receiver.name with no invoke)
//gets the same namespace split as calls. Called from
//Access(SnIdentifierExpr) once the plain search bound the inner name —
//that search walks the receiver's whole chain (own members first), so
//it is namespace-blind. Without the split, an own static shadows an
//inherited instance method, and a static-only name binds as a
//receiver-bearing handle (silent garbage at runtime).
bool ExprResolveAccessor::ApplyReceiverNamespaceFilter(
	SnIdentifierExpr &idExpr, SnField *&rpField)
{
	auto *pMember = AsMemberInner(idExpr);
	if (!pMember || rpField->Kind() != NK_Function)
		return true;
	auto *pOuter = pMember->Outer();
	if (OuterIsNamespaceQualifier(pOuter))
		return true;
	if (ReceiverNamesType(pOuter))
		return RejectTypeNameMethodValue(idExpr);
	if (!rpField->ContainFlags(NF_Static))
		return true;
	//Value receiver: the instance namespace is the only one it sees — a
	//shadowing own static gives way to the inherited instance method
	//(AllowAccess gates the rebind; FindMethodInNamespace does not).
	auto *pInstance = FindMethodInNamespace(m_pContext, idExpr.Name(), false);
	if (pInstance && pInstance->AllowAccess(*m_pAccessor))
	{
		rpField = pInstance;
		return true;
	}
	m_Env.Log(CLL_Error, idExpr.Location(),
		"the static method \"%s\" must be called through the type name.",
		idExpr.Name().c_str());
	return false;
}

//0.9.10 D2.4: classify the call site by receiver syntax. A bare call
//(any position other than the inner invoke of a member access) keeps the
//namespace-level lookup; a namespace qualifier is not a receiver object
//either (TryResolveModuleQualified owns that path).
InvokeReceiverForm ClassifyInvokeReceiver(const SnInvokeExpr &invoke)
{
	auto *pParent = invoke.Parent();
	if (!(pParent && pParent->Kind() == NK_MemberExpr))
		return InvokeReceiverForm::Bare;
	auto *pMember = static_cast<SnMemberExpr*>(pParent);
	if (pMember->Inner() != &invoke)
		return InvokeReceiverForm::Bare;
	auto *pOuter = pMember->Outer();
	if (pOuter && (pOuter->Kind() == NK_IdentifierExpr
		|| pOuter->Kind() == NK_MemberExpr))
	{
		auto *pField = static_cast<SnFieldExpr*>(pOuter)->Field();
		if (pField && pField->Kind() == NK_Namespace)
			return InvokeReceiverForm::Bare;
	}
	return ReceiverNamesType(pOuter)
		? InvokeReceiverForm::TypeName : InvokeReceiverForm::Value;
}

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

} //namespace nlang
