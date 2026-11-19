/*---
    ExprResolverStdLib.cpp — 库命名空间调用与跨模块限定名解析（TryResolveModuleQualified / 模块提示）。
    从 ExprResolver.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnTypes.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include "BindingTable.h"   //IsBuiltinHandleDeclName (D11), BindingPackageForName (D10)
#include <nlang/langservice/SymbolIndex.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include "CastInfo.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Close out a member whose resolution is decided: sentinel-bind an
//unwired callee, then mark resolved so no later phase re-reports the
//chain. Shared tail of every module-qualified path (diagnosed
//failures and the success tail alike) and of the void-receiver guard
//in Access. A diagnosed failure reaches here with the callee never
//bound, yet the resolved flag makes an enclosing member access trust
//the chain: SnFieldExpr::IsDataExpr() dereferences the null field and
//crashes ncc (0xC0000005) — hence the sentinel: the AST's meta SnType
//singleton (concrete, per-build — its NF_Type|NF_Field marks keep
//IsDataField false), so an enclosing chain degrades to a plain
//member-not-found. The sentinel only decorates diagnosed, never-
//codegen'd trees; it must be fetched per use (a function-local static
//would leak the previous build's node into the next).
void ExprResolveAccessor::FinishConsumedMember(
	SnMemberExpr &snMember)
{
	if (!snMember.m_pField)
	{
		auto *pSentinel = SnType::Instance();
		snMember.m_pField = pSentinel;
		snMember.EvalDataType(pSentinel);
	}
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

	//D-29 rule 1 (ruling 45): one question decides the claim — where the
	//module path ends. The chain addresses a module-level call when the
	//longest known module-path prefix of it is the whole chain (a package
	//function, `io.print`) or a strict prefix whose next segment is not a
	//type declared in that module. The type case (`lib.Mix.f`,
	//`http.HttpResponse.reasonPhraseFor`, `utils.helper.Widget.answer`) is a
	//qualified type access at any depth and the type-receiver path owns it —
	//a same-named type keeps its m12 priority over a same-named child
	//package, and the scan has to reach every known prefix to see it (the
	//first prefix alone let `utils.helper` claim `utils.helper.Widget`).
	//Everything else that extends a known module addresses an unimported
	//child package (`lib.sub.f`, spec §6.2: a parent import does not open
	//the child) and stays claimed: an unimported project child is exactly an
	//unregistered one, so existence is not checkable here, and
	//RejectUnimportedModuleCall names the path instead of letting the chain
	//descend into mid-chain members and die as follow-on noise.
	std::string knownPrefix;
	size_t tailStart = 0;
	if (!LongestKnownModulePrefix(pathSegs, knownPrefix, tailStart))
		return false;
	if (tailStart < pathSegs.size()
		&& m_Env.Registry().FindModuleType(knownPrefix,
			pathSegs[tailStart]) != nullptr)
		return false;
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
		FinishConsumedMember(snMember);
		return false;
	}
	if (!ValidateInvokeSyntax(invoke))
	{
		FinishConsumedMember(snMember);
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

	FinishConsumedMember(snMember);
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
	//m12 semantics: a USER declaration shadows the built-in surfaces.
	//The builtin declaration itself is not a user shadow — the declared
	//generic classes (Future, Channel, ...) are prelude members every
	//scope probe can see, and treating them as shadows would retire
	//the static faces (Future.selectAny) and every name-keyed arm that
	//shares this probe. D11: the builtin test is the shared predicate,
	//so a stamped package's handle classes stay exempt after the
	//concurrency classes leave the prelude.
	if (pField)
	{
		auto &registry = m_Env.Registry();
		const uint32_t owner = registry.OwnerOf(*pField);
		if (owner != ModuleRegistry::NO_OWNER
			&& IsBuiltinHandleDeclName(registry, owner, name))
			return false;
	}
	return pField != nullptr;
}

//spec §6.2 last line: after a failed resolution whose dotted name is also
//a module path, note the conflict (silent when none does).
void ExprResolveAccessor::MaybeLogModuleHint(const std::string &name)
{
	if (name.empty() || !m_Env.Registry().IsKnownModule(name))
		return;
	m_Env.Log(CLL_Error, "(note: a module path '%s' exists; module access "
		"requires an import and qualification)", name.c_str());
}

//D10 (concurrency import redesign), v0.12.7 V1 revision: an unqualified-name
//miss where a stamped package declares the name — name the package and
//the fix (the binding table is the package->symbol map for these
//classes). An explicit import now admits bare use for functions and
//classes alike (the old qualified-only function note was wrong for the
//unified model), so one text serves both forms. Suppressed when this
//TU already imports the package: the miss then has another cause (the
//"add import" advice would be noise). True = note logged (the caller
//suppresses the generic visibility/module-path guesses).
bool ExprResolveAccessor::MaybeLogPackageHint(const std::string &name)
{
	std::string package;
	if (!BindingPackageForName(name, package))
		return false;
	const uint32_t curModule =
		m_Env.Registry().OwnerOfContext(*m_pContext);
	if (curModule != ModuleRegistry::NO_OWNER
		&& m_Env.Registry().ImportGateAllows(curModule, package))
		return false;
	m_Env.Log(CLL_Error,
		"(note: \"%s\" is provided by package '%s'; "
		"add 'import %s';)",
		name.c_str(), package.c_str(), package.c_str());
	return true;
}

} //namespace nlang
