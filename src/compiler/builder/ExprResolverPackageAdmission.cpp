/*---
    ExprResolverPackageAdmission.cpp — 包调用准入策略与模块限定名被调方绑定。
    从 ExprResolverStdLib.cpp 抽取（合并期可维护性重构，零行为变化）。
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
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Package-call admission policy (user manual, standard-library chapter):
//the module-qualified path is a binding-policy domain of its own — every
//package formal admits per the conversion matrix (widening only), while
//user-function calls keep the general language rule that coerces
//scalar/array values into any string formal. The io coercing trio
//(print/write/eprint) is the documented exception: it accepts string,
//arrays, all scalar primitives and function values, converted at the
//call site. Keyed by package + name because the manual frames it as an
//io-specific exception, not a language surface — a `coerce` parameter
//modifier for three library functions would be speculative generality
//(and the declarations cannot express "this string formal coerces,
//that one is strict" any other way). A user package shadowing the io
//path inherits the policy on exactly these three names — a documented
//edge.
static bool IsIoCoercingTrioCall(const std::string &modulePath,
	const std::string &funcName)
{
	return modulePath == "io"
		&& (funcName == "print" || funcName == "write"
			|| funcName == "eprint");
}

//Coerces to string (the trio's accepted argument set): exactly the kinds the
//cast table adjudicates TCK_Auto against a string target — which is also the
//§4.4 implicit-conversion set and §5.3's rendering set: a string, a scalar
//primitive, an enumeration (its member name), an array, a class value (the
//virtual toString) or a function value (its own direct conversion). Enum and
//class values join the set because printing IS a text rendering: an enumeration
//wrapped here goes through the same conversion an assignment takes (the member
//name, not the numeric value), and a class value through its toString. Interface
//and struct values stay rejected — neither has a toString (an interface value
//has no members beyond its own declaration, a struct has none at all) — and so
//does Object, whose only paths to text are .toString() and the unboxing cast.
static bool CoercesToString(SnField *pArgType)
{
	if (!pArgType)
		return false;
	const NodeKind k = pArgType->Kind();
	if (k == NK_String || k == NK_ArrayTypeToken || k == NK_EnumDecl)
		return true;
	if (ScalarPrimIndexOf(k) >= 0)
		return true;
	if (k != NK_ClassDecl)
		return false;
	if (IsFuncTypeDecl(pArgType))
		return true;   //a func<...> handle formats directly
	return pArgType->Name() != "Object";
}

//One pending in-place wrap of AdmitCoercingTrioArgs: the argument
//expression and the child list that holds it (the invoke's own list, or
//a named arg's wrapper list).
struct TrioWrap
{
	SnExpression *pArg;
	ImmutableNodeList *pContainer;
};

//Locate the wrap target of one argument child: a named arg holds the
//value inside its own wrapper (the pattern of TryFixupNamedArgBinding);
//out args cannot bind a string formal and stay for the binder's own
//reject. False = this child has no wrap target.
static bool TrioWrapTarget(SnExpression &child, SnExpression *&rpArg,
	ImmutableNodeList *&rpContainer, ImmutableNodeList &children)
{
	if (child.Kind() == NK_NamedArgExpr)
	{
		auto &named = static_cast<SnNamedArgExpr&>(child);
		auto &innerChildren = const_cast<SnNamedArgExpr&>(named).Children();
		if (innerChildren.find(named.Inner()) == innerChildren.end())
			return false;
		rpArg = named.Inner();
		rpContainer = &innerChildren;
		return true;
	}
	if (child.Kind() == NK_OutArgExpr)
		return false;
	rpArg = &child;
	rpContainer = &children;
	return true;
}

//Coercing-trio arm of the admission policy: wrap every non-string
//argument that coerces to string in the implicit conversion IN PLACE
//(the same wrap FixupParamTypesWithBindings applies for user
//functions), so the call matches the declaration exactly instead of
//riding the generic binder's distance rung. Values with no implicit path
//to string are rejected here, each with the wording that fits its kind (an
//enum wrapped through this arm takes the same conversion an assignment
//takes, so it prints its member name and not the numeric value). True = the
//call may proceed to matching.
bool ExprResolveAccessor::AdmitCoercingTrioArgs(SnInvokeExpr &invoke)
{
	//Scan first, wrap after: FixupExprType erases the argument's list
	//node, so wrapping while iterating invoke.Children() would advance a
	//stale iterator (std::list erase invalidates exactly it) into nodes
	//other child lists reuse, scrambling the tree. The fresh find() per
	//wrap is the established discipline (TryFixupNamedArgBinding).
	std::vector<TrioWrap> wraps;
	auto &children = invoke.Children();
	size_t argOrdinal = 0;
	for (auto it = children.begin(); it != children.end(); ++it, ++argOrdinal)
	{
		SnExpression *pArg;
		ImmutableNodeList *pContainer;
		if (!TrioWrapTarget(static_cast<SnExpression&>(*it),
				pArg, pContainer, children))
			continue;
		auto *pArgType = pArg->EvalDataType();
		//Unresolved/void: its own resolution already reported.
		if (!pArgType || pArgType->Kind() == NK_String)
			continue;
		//Null is Int32-typed and coerces by kind, but wrapping it would
		//print "0" — leave it for the binder's named null policy.
		if (pArg->ContainFlags(NF_NullLiteral))
			continue;
		if (CoercesToString(pArgType))
			wraps.push_back({pArg, pContainer});
		else
		{
			LogArgumentTypeMismatch(*pArg, *pArgType,
				*SnBuiltinDataType::InstanceOf(NK_String),
				invoke.CalleeName(), (int)argOrdinal);
			return false;
		}
	}
	for (auto &wrap : wraps)
	{
		auto iArg = wrap.pContainer->find(wrap.pArg);
		NLANG_ASSERT(iArg != wrap.pContainer->end());
		NodeIterator iExpr = iArg;
		auto *pStringType = SnBuiltinDataType::InstanceOf(NK_String);
		TypeCastInfo castInfo(wrap.pArg->EvalDataType(), pStringType);
		FixupExprType(iExpr, castInfo);   //TCK_Auto by construction
	}
	return true;
}

//Strict arm of the admission policy: after a successful match, veto any
//binding the generic binder admitted through its →string distance rung
//(scalar/array/enum into a string formal) — package string formals take
//string only; the value must be converted explicitly at the call site
//(writeFile takes the content it writes). Runs post-match so the
//diagnostic names the matched declaration's formal, and after the
//coercing-trio arm has already wrapped its exceptions. True = a
//coercion was rejected (logged).
bool ExprResolveAccessor::RejectPackageStringCoercion(SnInvokeExpr &invoke,
	const std::vector<FormalBinding> &bindings,
	const std::string &modulePath)
{
	int paramIdx = 0;
	for (const auto &b : bindings)
	{
		if (b.kind != FormalBinding::B_Default && b.pCallerExpr)
		{
			auto *pFormalType = b.pFormal->EvalDataType();
			auto *pArgType = b.pCallerExpr->EvalDataType();
			if (pFormalType && pFormalType->Kind() == NK_String
				&& pArgType && pArgType->Kind() != NK_String)
			{
				m_Env.Log(CLL_Error, b.pCallerExpr->Location(),
					"Argument %d of \"%s.%s\" has type \"%s\"; a string "
					"is required — package parameters do not implicitly "
					"coerce to string.",
					paramIdx + 1, modulePath.c_str(),
					invoke.CalleeName().c_str(),
					pArgType->ToString().c_str());
				return true;
			}
		}
		++paramIdx;
	}
	return false;
}

//The import-flag hint of a failed qualified match mirrors the bare
//path (FindFuncByInvoke): consulted only for an unambiguous
//Incompatible — an ambiguity report is complete on its own, and a
//plain NotFound has no name-matched candidates to speak of.
static bool AnyNameMatchedImported(
	const std::vector<SnFunction*> &candidates)
{
	for (auto *pCandidate : candidates)
		if (pCandidate->ContainFlags(NF_Imported))
			return true;
	return false;
}

//Post-match close-out of the module-qualified call: out-argument veto,
//invoke binding, and the member's callee/result wiring. Codegen
//contract (VmBackend's MemberExpr handler): the resolved inner invoke
//is emitted as the bare call; the member carries its result type and
//the callee for chained access. False = a step rejected (already
//diagnosed).
bool ExprResolveAccessor::BindQualifiedInvoke(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, SnFunction &callee, FindFuncResult res,
	std::vector<FormalBinding> &bindings)
{
	if (OutArgOnDispatchedCalleeRejected(invoke, callee, bindings))
		return false;
	if (!ResolveInvokeWithFunc(invoke, callee, res, bindings))
		return false;
	snMember.m_pField = &callee;
	if (invoke.EvalDataType())
		snMember.EvalDataType(invoke.EvalDataType());
	return true;
}

//The failure arm of ResolveModuleQualifiedCallee: the qualified-ctor
//fallback (0.9.0: a member name that is one of the module's CLASSES is
//a qualified constructor call — functions keep priority, the match in
//the caller ran first), then the shared failure diagnostics (the
//ambiguity report suppresses the generic text, v0.12.7) and the
//consumed close-out. True = the ctor fallback bound the member.
bool ExprResolveAccessor::ReportQualifiedCalleeMiss(
	SnMemberExpr &snMember, SnInvokeExpr &invoke, FindFuncResult res,
	SnFunction *pCallee, bool bAmbiguous,
	const std::vector<SnFunction*> &candidates,
	const std::string &modulePath)
{
	if (res == FFR_FuncNameNotFound
		&& TryResolveQualifiedCtorInvoke(snMember, invoke, modulePath))
		return true;   //ctor bound; the caller finishes the member
	LogInvokeFailure(invoke, res, pCallee,
		res == FFR_Incompatible && !bAmbiguous
			&& AnyNameMatchedImported(candidates), candidates,
		bAmbiguous);
	FinishConsumedMember(snMember);
	return false;
}

//Candidate matching and binding of the module-qualified call, shared
//close-out discipline of the bare path. True = fully resolved (the
//member carries callee and result type; the caller only finishes the
//resolved flags); false = a failure was diagnosed and the member
//consumed (match failure, admission-policy reject, out-argument reject,
//or a function-reference argument that failed to bind).
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
	//Admission policy, coercing-trio arm first: the wraps must precede
	//matching so coercing values land as exact string matches.
	const bool bCoercingTrio =
		IsIoCoercingTrioCall(modulePath, invoke.CalleeName());
	if (bCoercingTrio && !AdmitCoercingTrioArgs(invoke))
	{
		FinishConsumedMember(snMember);
		return false;
	}
	auto res = MatchInvokeAgainst(invoke, candidates, pCallee, bindings,
		bAmbiguous);
	if (res != FFR_ExactMatch && res != FFR_ApproximateMatch)
		return ReportQualifiedCalleeMiss(snMember, invoke, res, pCallee,
			bAmbiguous, candidates, modulePath);
	//Admission policy, strict arm: veto the generic binder's →string
	//coercions on every non-trio package call.
	if (!bCoercingTrio
		&& RejectPackageStringCoercion(invoke, bindings, modulePath))
	{
		FinishConsumedMember(snMember);
		return false;
	}
	if (!BindQualifiedInvoke(snMember, invoke, *pCallee, res, bindings))
	{
		FinishConsumedMember(snMember);
		return false;
	}
	return true;
}

} //namespace nlang
