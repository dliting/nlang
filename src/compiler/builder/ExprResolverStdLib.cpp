/*---
    ExprResolverStdLib.cpp — 库命名空间调用与跨模块限定名解析（TryResolveModuleQualified / 模块提示）。
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
#include <nlang/langservice/SymbolIndex.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include "CastInfo.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Mark the member resolved and bind the array token — the shared
//close-out of every module-qualified path (diagnosed failures and the
//success tail alike), so no later phase re-reports the chain.
void ExprResolveAccessor::FinishModuleQualifiedMember(
	SnMemberExpr &snMember)
{
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
}

//Decline tests of the module-qualified fallback: the chain shape (plain
//identifiers with the invoke at the tip; qualified VALUE access is out
//of the v1 surface), the m12 priority test (a local / field / type with
//the LEFTMOST name wins — the module table is only a fallback for names
//that resolve as nothing else; function candidates are excluded by the
//probe, so a cross-directory function name never shadows a module
//path's first segment, and a same-named class wins, spec §6.2 last
//line) and the module-table lookup itself. True = the chain addresses
//a known module (rpInvoke/rModulePath out); false = decline, the normal
//path keeps the expression.
bool ExprResolveAccessor::TryResolveModuleCallTarget(
	SnMemberExpr &snMember, SnInvokeExpr *&rpInvoke,
	std::string &rModulePath)
{
	if (!snMember.Outer() || !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_InvokeExpr)
		return false;

	const std::vector<std::string> pathSegs =
		OuterIdentifierChain(snMember);
	if (pathSegs.empty())
		return false;
	rModulePath = JoinDots(pathSegs);
	rpInvoke = &static_cast<SnInvokeExpr &>(*snMember.Inner());

	if (ProbeNonFunctionField(pathSegs.front())
		|| IsBuiltinClassName(pathSegs.front()))
		return false;

	if (m_Env.Registry().IsKnownModule(rModulePath))
		return true;
	//A library namespace the symbol index knows but this build did not
	//inline (the caller never imported it, so DiscoverLibraryUnits did not
	//pull it in) is still a library target — RejectUnimportedModuleCall
	//names the missing import instead of a spurious field error.
	return m_Env.IsLibraryPackage(pathSegs.front());
}

//Spec §7 row 1: the module is known but not imported into the current
//TU — name the fix and consume the chain here (normal resolution would
//only add "Cannot resolve the field" for the leftmost identifier).
//True = rejected and consumed; false = imported, processing continues.
bool ExprResolveAccessor::RejectUnimportedModuleCall(
	SnMemberExpr &snMember, const std::string &modulePath)
{
	auto &reg = m_Env.Registry();
	if (reg.IsModuleImported(reg.OwnerOfContext(*m_pContext), modulePath))
		return false;
	if (modulePath.find('.') == std::string::npos)
	{
		//Single-segment path. A library package (io/math/fs or a
		//third-party package) reads "Package"; an external .ncu reads
		//"Module". No wildcard is suggested — it never matches a
		//single-segment name (§3.3); the exact form always suffices.
		const char *pKind = m_Env.IsLibraryPackage(modulePath)
			? "Package" : "Module";
		m_Env.Log(CLL_Error, snMember.Location(),
			"%s '%s' is not imported. Add 'import %s;' at the top "
			"of this file.", pKind, modulePath.c_str(), modulePath.c_str());
	}
	else
	{
		const size_t lastDot = modulePath.find_last_of('.');
		const std::string parentPrefix =
			modulePath.substr(0, lastDot);
		m_Env.Log(CLL_Error, snMember.Location(),
			"Module '%s' is not imported. Add 'import %s;' (or "
			"'import %s.*;') at the top of this file.",
			modulePath.c_str(), modulePath.c_str(),
			parentPrefix.c_str());
	}
	FinishModuleQualifiedMember(snMember);
	return true;
}

//Argument handling mirrors Access(SnInvokeExpr), in the same order —
//params resolve first, then the caller-side syntax check (the caller
//context is still active here; the receiver scope switch happens
//later). False = a failure was diagnosed and the member consumed;
//true = both steps passed.
bool ExprResolveAccessor::TryResolveModuleQualifiedArgs(
	SnMemberExpr &snMember, SnInvokeExpr &invoke)
{
	if (!ResolveExpressionList(invoke.Params()))
	{
		FinishModuleQualifiedMember(snMember);
		return false;
	}
	if (!ValidateInvokeSyntax(invoke))
	{
		FinishModuleQualifiedMember(snMember);
		return false;
	}
	return true;
}


//Module import visibility (spec §6.2 rule 5): the module-table fallback
//for dotted call chains. Runs BEFORE the outer identifier resolves, so a
//module-path diagnostic never doubles with a spurious "Cannot resolve the
//field". Declines (returns false) whenever the chain belongs to something
//else — the normal path then keeps the expression; every matching branch
//consumes the member (resolved or diagnosed).
bool ExprResolveAccessor::TryResolveModuleQualified(SnMemberExpr &snMember)
{
	SnInvokeExpr *pInvoke = nullptr;
	std::string modulePath;
	if (!TryResolveModuleCallTarget(snMember, pInvoke, modulePath))
		return false;
	auto &invoke = *pInvoke;

	if (RejectUnimportedModuleCall(snMember, modulePath))
		return true;

	if (!TryResolveModuleQualifiedArgs(snMember, invoke))
		return true;

	if (!ResolveModuleQualifiedCallee(snMember, invoke, modulePath))
		return true;

	FinishModuleQualifiedMember(snMember);
	return true;
}

//Shape split of a qualified-value chain (the file-local prologue of
//TryResolveQualifiedTypeValue): pkg[.sub].Type.member — the member is
//the last segment, the type the second-to-last, everything before them
//the module path. False when the chain is shorter than pkg.Type.member
//(no shorter value shape exists).
static bool SplitQualifiedName(std::vector<std::string> &rSegs,
	std::string &rModulePath, std::string &rTypeName,
	std::string &rMemberName)
{
	if (rSegs.size() < 3)
		return false;
	rMemberName = rSegs.back();
	rTypeName = rSegs[rSegs.size() - 2];
	for (size_t i = 0; i + 2 < rSegs.size(); ++i)
	{
		if (!rModulePath.empty())
			rModulePath += ".";
		rModulePath += rSegs[i];
	}
	return true;
}

//Phase 5: package-qualified VALUE access — the `alib.Color.Green`
//enum-constant chain, resolved through the registry (see the header
//declaration for the route contract). True = consumed (resolved or
//diagnosed); false = declined, the generic path keeps the expression.
bool ExprResolveAccessor::TryResolveQualifiedTypeValue(SnMemberExpr &snMember)
{
	std::vector<std::string> segs = OuterIdentifierChain(snMember);
	if (segs.empty()
		|| !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_IdentifierExpr)
		return false;
	segs.push_back(
		static_cast<SnIdentifierExpr *>(snMember.Inner())->Name());

	//m12 priority: the leftmost local/field/type wins (as for calls).
	if (ProbeNonFunctionField(segs.front())
		|| IsBuiltinClassName(segs.front()))
		return false;

	std::string modulePath, typeName, memberName;
	if (!SplitQualifiedName(segs, modulePath, typeName, memberName))
		return false;

	auto &reg = m_Env.Registry();
	if (!reg.IsKnownModule(modulePath)
		&& !m_Env.IsLibraryPackage(segs.front()))
		return false;

	if (RejectUnimportedModuleCall(snMember, modulePath))   //import gate, call wording
		return true;

	SnField *pType = reg.FindModuleType(modulePath, typeName);
	if (pType == nullptr)
	{
		m_Env.Log(CLL_Error, snMember.Location(),
			"Type '%s' is not a member of package '%s'.",
			typeName.c_str(), modulePath.c_str());
		FinishModuleQualifiedMember(snMember);
		return true;
	}
	if (pType->Kind() != NK_EnumDecl)
		return false;   //value members of other type kinds: the generic path

	SnField *pMember = pType->FindField(memberName);   //miss: generic path names it
	if (pMember == nullptr)
		return false;

	//Bind tip + member exactly like FinishResolvedMember does.
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Inner()),
		pMember);
	ResolveFieldExprAs(snMember, pMember);
	FinishModuleQualifiedMember(snMember);
	return true;
}

//Non-function probe for the m12 priority test: true when the name
//resolves as a local / field / type in the current context. A FUNCTION
//with the same name counts as a miss — function names must not shadow a
//module path's first segment. using-imported namespaces are not probed
//(spec §5.2 rule 7: using never applies to module paths), so a user
//namespace sharing an exact module path name is outside the v1 surface.
bool ExprResolveAccessor::ProbeNonFunctionField(const std::string &name)
{
	auto *pField = FindFieldInAncestor(name, *m_pContext, *m_pAccessor,
		Flags());
	if (pField && pField->Kind() == NK_Function)
		return false;
	//An inlined library namespace is reached through the module-qualified
	//path, not as a non-function field that declines that path.
	if (pField && pField->Kind() == NK_Namespace
		&& m_Env.Registry().IsLibraryModule(
			m_Env.Registry().OwnerOf(*pField)))
		return false;
	return pField != nullptr;
}

//spec §6.2 last line: after a failed resolution whose dotted name is also
//a module path, note the conflict (silent when none does).
void ExprResolveAccessor::MaybeLogModuleHint(const std::string &name)
{
	if (name.empty() || !m_Env.Registry().HasKnownModuleStartingWith(name))
		return;
	m_Env.Log(CLL_Error, "(note: a module path '%s' exists; module access "
		"requires an import and qualification)", name.c_str());
}

} //namespace nlang
