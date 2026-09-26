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

//Phase 11: namespace-qualified stdlib call (math.sqrt(x), io.print(s)).
//Resolves against the built-in table in StdLib.h. Every branch consumes
//the expression — resolved or diagnosed — because namespace names are
//reserved and never resolve as fields (no fallback path exists).
void ExprResolveAccessor::TryResolveStdLibCall(SnMemberExpr &snMember,
	SnIdentifierExpr &outerId, SnInvokeExpr &invoke)
{
	const std::string ns(outerId.Name());
	const auto& fnName = invoke.CalleeName();

	//Table-driven by-name dispatch: named/out arguments can never bind
	//(same guards as the built-in string methods in Access(SnMemberExpr&)).
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Named arguments are not supported by standard library functions.");
		return;
	}
	if (HasOutArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"out arguments are not supported by standard library functions.");
		return;
	}

	const StdLibEntry* pEntry = FindStdLibFunction(ns, fnName);
	if (!pEntry)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Unknown standard library function \"%s.%s\".",
			ns.c_str(), fnName.c_str());
		return;
	}

	const size_t argCount = ArgCountOf(invoke);
	if (argCount < pEntry->minArgs || argCount > pEntry->maxArgs)
	{
		if (pEntry->minArgs == pEntry->maxArgs)
			m_Env.Log(CLL_Error, invoke.Location(),
				"\"%s.%s\" expects %d argument(s).",
				ns.c_str(), fnName.c_str(), (int)pEntry->minArgs);
		else
			m_Env.Log(CLL_Error, invoke.Location(),
				"\"%s.%s\" expects %d to %d argument(s).",
				ns.c_str(), fnName.c_str(),
				(int)pEntry->minArgs, (int)pEntry->maxArgs);
		return;
	}

	//Args resolve in the caller's scope. The intercept runs before
	//Access(SnMemberExpr&) sets ERF_SearchInParentOnly / swaps m_pContext,
	//so no context restore is needed (unlike the string-methods branch).
	ResolveExpressionList(invoke.Params());

	//Per-param type policy: exact RTK kind match, or int->float widening
	//(wrapped in a cast expr in place — the FixupParamTypesWithBindings
	//recipe over invoke.Children()). Everything else is a compile
	//error naming the function, so the user sees which call is wrong.
	auto& children = invoke.Children();
	size_t paramIdx = 0;
	for (auto it = children.begin(); it != children.end(); ++it, ++paramIdx)
	{
		auto& arg = static_cast<SnExpression&>(*it);
		auto* pArgType = arg.EvalDataType();
		if (!pArgType)
		{
			//A resolved arg with no type is a void call (the assignment
			//statement guards the same shape): passing it would silently
			//stage a stale pResult in the claim slot.
			if (arg.IsResolved())
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" has no value: a void function "
					"result cannot be used as an argument.",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str());
			continue;  //unresolved arg was diagnosed above
		}
		const NodeKind argKind = pArgType->Kind();
		const uint8_t want = pEntry->paramKinds[paramIdx];
		//io.print (coerceToString): every param accepts string|int|float|
		//array — codegen branches on the arg's own static kind and
		//converts at the call site (OP_Array_to_str for tokens). No
		//widening wrap here; class/struct/enum must call .toString()
		//explicitly.
		if (pEntry->coerceToString)
		{
			//null literal is Int32-typed (KT_Null); accepting it would
			//print "0". Reject it explicitly.
			if (arg.ContainFlags(NF_NullLiteral))
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" cannot be null.",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str());
			}
			else if (argKind != NK_String && argKind != NK_Int32
				&& argKind != NK_Float
				&& argKind != NK_ArrayTypeToken
				//Phase 13: function handles print through the direct
				//conversion path ("func <name>") like the other three
				//toString routes.
				&& !IsFuncTypeDecl(pArgType))
			{
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of \"%s.%s\" has type \"%s\"; string, int "
					"or float expected (class and enum values: call "
					".toString() first).",
					(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
					pArgType->ToString().c_str());
			}
			continue;
		}
		bool ok = (argKind == NK_Int32 && want == RTK_Int32)
			|| (argKind == NK_Float && want == RTK_Float)
			|| (argKind == NK_String && want == RTK_String);
		const bool widen = (argKind == NK_Int32 && want == RTK_Float);
		if (!ok && !widen)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of \"%s.%s\" has type \"%s\"; \"%s\" expected.",
				(int)paramIdx + 1, ns.c_str(), fnName.c_str(),
				pArgType->ToString().c_str(), StdLibKindName(want));
			continue;
		}
		if (widen)
		{
			//Sole automatic promotion (same policy as user-function calls).
			auto* pFloatType = SnBuiltinDataType::InstanceOf(NK_Float);
			TypeCastInfo castInfo(pArgType, pFloatType);
			FixupExprType(it, castInfo);
		}
	}

	//Wrap up. invoke.Callee() deliberately stays null — same as the built-in
	//string methods — so the walker reserves argCount+1 slots; the
	//namespace-shaped emission only uses argCount (over-reserve is safe).
	invoke.AddFlags(NF_Resolved);
	SnField* pResultField = nullptr;
	switch ((StdLibReturnType)pEntry->returnType)
	{
	case SLRT_Float:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Float);
		break;
	case SLRT_Int32:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Int32);
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

//Module import visibility (spec §6.2 rule 5): the module-table fallback
//for dotted call chains. Runs BEFORE the outer identifier resolves, so a
//module-path diagnostic never doubles with a spurious "Cannot resolve the
//field". Declines (returns false) whenever the chain belongs to something
//else — the normal path then keeps the expression; every matching branch
//consumes the member (resolved or diagnosed).
bool ExprResolveAccessor::TryResolveModuleQualified(SnMemberExpr &snMember)
{
	//Shape gate: the chain must be plain identifiers with the invoke at
	//the tip. Qualified VALUE access (utils.helper as a value) is out of
	//the v1 surface and falls through to normal resolution.
	if (!snMember.Outer() || !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_InvokeExpr)
		return false;

	const std::vector<std::string> pathSegs =
		OuterIdentifierChain(snMember);
	if (pathSegs.empty())
		return false;
	const std::string modulePath = JoinDots(pathSegs);
	auto &invoke = static_cast<SnInvokeExpr &>(*snMember.Inner());

	//Priority test (m12): a local / field / type with the LEFTMOST name
	//wins — the module table is only a fallback for names that resolve as
	//nothing else. Function candidates are excluded by the probe, so a
	//cross-directory function name never shadows a module path's first
	//segment; a same-named class wins (spec §6.2 last line).
	if (ProbeNonFunctionField(pathSegs.front())
		|| IsBuiltinClassName(pathSegs.front()))
		return false;

	auto &reg = m_Env.Registry();
	if (!reg.IsKnownModule(modulePath))
		return false;
	if (!reg.IsModuleImported(reg.OwnerOfContext(*m_pContext), modulePath))
	{
		//Spec §7 row 1. Consume the chain here: normal resolution would
		//only add "Cannot resolve the field" for the leftmost identifier.
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
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}

	//Argument handling mirrors Access(SnInvokeExpr), in the same order —
	//params resolve first, then the caller-side syntax check (the caller
	//context is still active here; the receiver scope switch happens
	//later). The remaining order difference to the bare path — failure
	//logging before the out-argument guard — is unobservable: a logged
	//failure means no viable callee, while the guard only runs on one.
	if (!ResolveExpressionList(invoke.Params()))
	{
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}
	if (!ValidateInvokeSyntax(invoke))
	{
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}

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
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}

	if (OutArgOnDispatchedCalleeRejected(invoke, *pCallee, bindings))
	{
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}

	if (!ResolveInvokeWithFunc(invoke, *pCallee, res, bindings))
	{
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		return true;
	}

	//Codegen contract (VmBackend's MemberExpr handler): the resolved inner
	//invoke is emitted as the bare call; the member carries its result
	//type and the callee for chained access.
	snMember.m_pField = pCallee;
	if (invoke.EvalDataType())
		snMember.EvalDataType(invoke.EvalDataType());
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
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
