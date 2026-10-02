/*---
    ExprResolverStdLib.cpp — 标准库命名空间调用与跨模块限定名解析（TryResolveStdLibCall / TryResolveModuleQualified / 模块提示）。
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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//A resolved stdlib arg with no type is a void call (the assignment
//statement guards the same shape): passing it would silently stage a
//stale pResult in the claim slot. True = the diagnostic fired.
static bool MaybeLogVoidStdLibArg(BuildEnvironment &env,
	SnExpression &arg, size_t paramIdx, const std::string &ns,
	const std::string &fnName)
{
	if (!arg.IsResolved())
		return false;
	env.Log(CLL_Error, arg.Location(),
		"Argument %d of \"%s.%s\" has no value: a void function "
		"result cannot be used as an argument.",
		(int)paramIdx + 1, ns.c_str(), fnName.c_str());
	return true;
}

//coerceToString branch (the io write/print/eprint trio) of the
//per-param gate: every param
//accepts string|int|float|array — codegen branches on the arg's own
//static kind and converts at the call site (OP_Array_to_str for
//tokens). No widening wrap here; class/struct/enum must call
//.toString() explicitly. Diagnostics only; the caller continues with
//the next argument either way.
static void RejectCoercedStringArg(BuildEnvironment &env,
	SnExpression &arg, SnField *pArgType, size_t paramIdx,
	const std::string &ns, const std::string &fnName)
{
	//null literal is Int32-typed (KT_Null); accepting it would
	//print "0". Reject it explicitly.
	if (arg.ContainFlags(NF_NullLiteral))
	{
		env.Log(CLL_Error, arg.Location(),
			"Argument %d of \"%s.%s\" cannot be null.",
			(int)paramIdx + 1, ns.c_str(), fnName.c_str());
		return;
	}
	//Phase 13: function handles print through the direct conversion
	//path ("func <name>") like the other toString routes.
	//0.7.5: the scalar set is registry-driven — bool prints as
	//true/false, and the P4-P6 kinds join without touching this gate.
	const NodeKind argKind = pArgType->Kind();
	if (argKind != NK_String && argKind != NK_ArrayTypeToken
		&& ScalarPrimIndexOf(argKind) < 0
		&& !IsFuncTypeDecl(pArgType))
	{
		env.Log(CLL_Error, arg.Location(),
			"Argument %d of \"%s.%s\" has type \"%s\"; string or a "
			"primitive value expected (class and enum values: call "
			".toString() first).",
			(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
			pArgType->ToString().c_str());
	}
}

//One stdlib argument vs its declared kind (0.7.5 scalar-matrix policy):
//TCK_Same admits as-is, a widening TCK_Auto admits wrapped in a cast
//expr in place (the FixupParamTypesWithBindings recipe); narrowing
//stays explicit-only. String params keep the exact-kind policy (the
//scalar registry does not know NK_String).
bool ExprResolveAccessor::ScalarArgAdmitted(NodeIterator &it,
	SnField* pArgType, uint8_t want)
{
	const int wantIdx = ScalarPrimIndexOfRtk(want);
	const int argIdx = ScalarPrimIndexOf(pArgType->Kind());
	if (wantIdx >= 0 && argIdx >= 0)
	{
		auto* pWantType =
			SnBuiltinDataType::InstanceOf(kScalarPrims[wantIdx].kind);
		TypeCastInfo castInfo(pArgType, pWantType);
		const TypeCastKind verdict = castInfo.Kind();
		if (verdict == TCK_Same)
			return true;
		if (verdict == TCK_Auto)
		{
			FixupExprType(it, castInfo);  //wrap result ignored, as before
			return true;
		}
		return false;
	}
	return pArgType->Kind() == NK_String && want == RTK_String;
}

//Per-param type policy of TryResolveStdLibCall: the per-argument
//admission above plus the error naming the function, so the user sees
//which call is wrong.
void ExprResolveAccessor::CheckStdLibParamTypes(SnInvokeExpr &invoke,
	const std::string &ns, const std::string &fnName,
	const StdLibEntry *pEntry)
{
	auto& children = invoke.Children();
	size_t paramIdx = 0;
	for (auto it = children.begin(); it != children.end(); ++it, ++paramIdx)
	{
		auto& arg = static_cast<SnExpression&>(*it);
		auto* pArgType = arg.EvalDataType();
		if (!pArgType)
		{
			MaybeLogVoidStdLibArg(m_Env, arg, paramIdx, ns, fnName);
			continue;  //unresolved arg was diagnosed above
		}
		const uint8_t want = pEntry->paramKinds[paramIdx];
		if (pEntry->coerceToString)
		{
			RejectCoercedStringArg(m_Env, arg, pArgType, paramIdx, ns,
				fnName);
			continue;
		}
		if (!ScalarArgAdmitted(it, pArgType, want))
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of \"%s.%s\" has type \"%s\"; \"%s\" expected.",
				(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
				pArgType->ToString().c_str(), StdLibKindName(want));
			continue;
		}
	}
}

//Result binding of TryResolveStdLibCall: the return-type switch and
//the member/channel writes. invoke.Callee() deliberately stays null —
//same as the built-in string methods — so the walker reserves
//argCount+1 slots; the namespace-shaped emission only uses argCount
//(over-reserve is safe).
void ExprResolveAccessor::BindStdLibCallResult(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, const StdLibEntry *pEntry)
{
	invoke.AddFlags(NF_Resolved);
	SnField* pResultField = nullptr;
	switch ((StdLibReturnType)pEntry->returnType)
	{
	case SLRT_Float:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Float);
		break;
	case SLRT_Double:  //0.7.5: math at double precision
		pResultField = SnBuiltinDataType::InstanceOf(NK_Double);
		break;
	case SLRT_Long:    //0.7.5: math.floor/ceil/round
		pResultField = SnBuiltinDataType::InstanceOf(NK_Long);
		break;
	case SLRT_Int32:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Int32);
		break;
	case SLRT_Bool:   //0.7.5: predicates
		pResultField = SnBuiltinDataType::InstanceOf(NK_Bool);
		break;
	case SLRT_String:
		pResultField = SnBuiltinDataType::InstanceOf(NK_String);
		break;
	case SLRT_ListString:
		//fs.listFiles / s.split (Step 3+): List<string> generic instance.
	{
		std::vector<SnField*> listArgs{
			SnBuiltinDataType::InstanceOf(NK_String) };
		pResultField = GetGenericClassDecl("List", listArgs, {},
			invoke.Location());
		break;
	}
	case SLRT_Void:
		break;  //void: no result type; void assignment rejected downstream
	}
	if (pResultField)
	{
		snMember.EvalDataType(pResultField);
		//Set m_pField directly (not via ResolveFieldExprAs) so chained
		//access (fs.join(a, b).length()) survives IsDataExpr().
		snMember.m_pField = pResultField;
	}
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
}

//Lookup gate of TryResolveStdLibCall: table-driven by-name dispatch
//rejects named and out arguments outright (same guards as the built-in
//string methods in Access(SnMemberExpr&)), then the table lookup and
//the arity range check run. Null = a diagnostic is logged and the call
//is consumed.
const StdLibEntry *ExprResolveAccessor::FindStdLibEntry(
	SnInvokeExpr &invoke, const std::string &ns, const std::string &fnName)
{
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Named arguments are not supported by standard library functions.");
		return nullptr;
	}
	if (HasOutArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"out arguments are not supported by standard library functions.");
		return nullptr;
	}
	const StdLibEntry* pEntry = FindStdLibFunction(ns, fnName);
	if (!pEntry)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Unknown standard library function \"%s.%s\".",
			ns.c_str(), fnName.c_str());
		return nullptr;
	}
	const size_t argCount = ArgCountOf(invoke);
	if (argCount >= pEntry->minArgs && argCount <= pEntry->maxArgs)
		return pEntry;
	if (pEntry->minArgs == pEntry->maxArgs)
		m_Env.Log(CLL_Error, invoke.Location(),
			"\"%s.%s\" expects %d argument(s).",
			ns.c_str(), fnName.c_str(), (int)pEntry->minArgs);
	else
		m_Env.Log(CLL_Error, invoke.Location(),
			"\"%s.%s\" expects %d to %d argument(s).",
			ns.c_str(), fnName.c_str(),
			(int)pEntry->minArgs, (int)pEntry->maxArgs);
	return nullptr;
}

//Phase 11: namespace-qualified stdlib call (math.sqrt(x), io.print(s)).
//Resolves against the built-in table in StdLib.h. Every branch consumes
//the expression — resolved or diagnosed — because namespace names are
//reserved and never resolve as fields (no fallback path exists).
void ExprResolveAccessor::TryResolveStdLibCall(SnMemberExpr &snMember,
	SnIdentifierExpr &outerId, SnInvokeExpr &invoke)
{
	const std::string ns(outerId.Name());
	const auto& fnName = invoke.CalleeName();

	const StdLibEntry *pEntry = FindStdLibEntry(invoke, ns, fnName);
	if (!pEntry)
		return;

	//Args resolve in the caller's scope. The intercept runs before
	//Access(SnMemberExpr&) sets ERF_SearchInParentOnly / swaps m_pContext,
	//so no context restore is needed (unlike the string-methods branch).
	ResolveExpressionList(invoke.Params());

	CheckStdLibParamTypes(invoke, ns, fnName, pEntry);
	BindStdLibCallResult(snMember, invoke, pEntry);
}

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

	return m_Env.Registry().IsKnownModule(rModulePath);
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
		//Single-segment path. Only the EXTERNAL .nmod case is why no
		//wildcard is suggested — a wildcard never matches an external
		//single-segment name (§3.3). A project ROOT module would
		//additionally be reachable via `import <name>.*;` (D11 union);
		//the exact form always suffices, so the message stays minimal.
		m_Env.Log(CLL_Error, snMember.Location(),
			"Module '%s' is not imported. Add 'import %s;' at the top "
			"of this file.", modulePath.c_str(), modulePath.c_str());
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

//Candidate matching and binding of the module-qualified call, shared
//close-out discipline of the bare path. True = fully resolved (the
//member carries callee and result type; the caller only finishes the
//resolved flags); false = a failure was diagnosed and the member
//consumed (match failure, out-argument reject, or a function-reference
//argument that failed to bind).
bool ExprResolveAccessor::ResolveModuleQualifiedCallee(
	SnMemberExpr &snMember, SnInvokeExpr &invoke,
	const std::string &modulePath)
{
	auto &reg = m_Env.Registry();
	std::vector<SnFunction*> candidates =
		reg.ModuleFunctions(modulePath, invoke.CalleeName());
	SnFunction *pCallee = nullptr;
	std::vector<FormalBinding> bindings;
	bool bAmbiguous = false;
	auto res = MatchInvokeAgainst(invoke, candidates, pCallee, bindings,
		bAmbiguous);
	if (res != FFR_ExactMatch && res != FFR_ApproximateMatch)
	{
		//Same contract as the bare path (FindFuncByInvoke): the imported
		//flag is only consulted for an unambiguous Incompatible — an
		//ambiguity report is complete on its own, and a plain NotFound has
		//no name-matched candidates to speak of.
		bool bNameMatchedImported = false;
		if (res == FFR_Incompatible && !bAmbiguous)
		{
			for (auto *pCandidate : candidates)
				if (pCandidate->ContainFlags(NF_Imported))
					bNameMatchedImported = true;
		}
		LogInvokeFailure(invoke, res, pCallee, bNameMatchedImported);
		FinishModuleQualifiedMember(snMember);
		return false;
	}
	if (OutArgOnDispatchedCalleeRejected(invoke, *pCallee, bindings))
	{
		FinishModuleQualifiedMember(snMember);
		return false;
	}
	if (!ResolveInvokeWithFunc(invoke, *pCallee, res, bindings))
	{
		FinishModuleQualifiedMember(snMember);
		return false;
	}
	//Codegen contract (VmBackend's MemberExpr handler): the resolved inner
	//invoke is emitted as the bare call; the member carries its result
	//type and the callee for chained access.
	snMember.m_pField = pCallee;
	if (invoke.EvalDataType())
		snMember.EvalDataType(invoke.EvalDataType());
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
