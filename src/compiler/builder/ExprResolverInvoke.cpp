/*---
    ExprResolverInvoke.cpp — 调用解析（参数前置、成功尾、失败诊断）
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
    2026-10-12 source-size 拆分：候选收集与重载匹配迁入
    ExprResolverInvokeMatch.cpp。
---*/
#include "ExprResolver.h"
#include "RefBindJudgment.h"
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
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Phase 13: delegate call — the callee name resolves to a func-typed
//value (local / param / class field) rather than a function. Name
//lookup order puts the func value first (Python-style shadowing of a
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
//is a frame-shift bug — codegen's bare-invoke path emits OP_CallStatic
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
	//0.9.10: a static callee has no receiver expectation — the bare form
	//emits OP_CallStatic slotBase 0 against a frame with no __this slot.
	if (pCallee->ContainFlags(NF_Static))
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

//0.9.10: an instance method cannot be reached through the type name —
//`Util.inst()` has no receiver value (before this gate the shape slipped
//through the resolver and crashed the backend's receiver emission).
//A bare enum name is the same shape (`Color.rank()`); the enum VALUE
//receiver (`Color.Blue.rank()`) has a MemberExpr outer and is unaffected.
//True = rejected (diagnostic logged).
bool ExprResolveAccessor::RejectTypeNameInstanceCall(SnInvokeExpr &snInvoke,
	SnFunction *pCallee)
{
	if (!pCallee || pCallee->ContainFlags(NF_Static))
		return false;
	auto *pInvokeParent = snInvoke.Parent();
	if (!(pInvokeParent && pInvokeParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr*>(pInvokeParent)->Inner() == &snInvoke))
		return false;
	auto *pOuter = static_cast<SnMemberExpr*>(pInvokeParent)->Outer();
	if (!(pOuter && pOuter->Kind() == NK_IdentifierExpr))
		return false;
	auto *pOuterField = static_cast<SnIdentifierExpr*>(pOuter)->Field();
	if (!(pOuterField && (pOuterField->Kind() == NK_ClassDecl
		|| pOuterField->Kind() == NK_InterfaceDecl
		|| pOuterField->Kind() == NK_EnumDecl)))
		return false;
	m_Env.Log(CLL_Error, snInvoke.Location(),
		"the instance method \"%s\" must be called through a receiver "
		"object.",
		pCallee->Name().c_str());
	return true;
}

//0.9.10 D2.4: the namespace-filtered search found no callee, but the
//other namespace holds a same-name member on the receiver's chain — the
//call form is wrong, not the name. Keeps the pre-existing type-name
//wording and mirrors it for a static reached through an object.
bool ExprResolveAccessor::RejectCrossNamespaceCall(SnInvokeExpr &snInvoke)
{
	const InvokeReceiverForm form = ClassifyInvokeReceiver(snInvoke);
	if (form == InvokeReceiverForm::Bare)
		return false;
	const bool bTypeName = form == InvokeReceiverForm::TypeName;
	if (!FindMethodInNamespace(m_pContext, snInvoke.CalleeName(), !bTypeName))
		return false;
	if (bTypeName)
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"the instance method \"%s\" must be called through a receiver "
			"object.",
			snInvoke.CalleeName().c_str());
	else
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"the static method \"%s\" must be called through the type name.",
			snInvoke.CalleeName().c_str());
	return true;
}

//The deferred-shape test both collectors share. 0.9.0: an untyped shape
//whose target type can only be the chosen overload's formal — the bare
//brace argument, and (0.13.10) the lambda. Empty for every other
//argument shape. 0.9.13 B2: a named argument `name = {...}` is judged on
//its inner expression; the SnNamedArgExpr wrapper stays in the list (the
//skip-list matches the owning list's entries by identity).
static bool IsDeferredShape(SnExpression &arg)
{
	SnExpression *pTest = &arg;
	if (arg.Kind() == NK_NamedArgExpr)
		pTest = static_cast<SnNamedArgExpr&>(arg).Inner();
	return UntypedInitListOf(*pTest) || UntypedLambdaOf(*pTest);
}

std::vector<SnExpression*> CollectDeferredArgs(
	SnInvokeExpr &invoke)
{
	std::vector<SnExpression*> deferredArgs;
	for (auto &param : invoke.Params())
		if (IsDeferredShape(param))
			deferredArgs.push_back(&param);
	return deferredArgs;
}

//v0.13.13a D-2(b): the new-expression twin (`new T(args)` and the
//generic instantiation `T<A>(args)`) — the same untyped shapes over the
//ctor actuals. The class-name child hangs in the Args() view but is not
//an argument.
std::vector<SnExpression*> CollectDeferredArgs(
	SnNewExpr &newExpr)
{
	std::vector<SnExpression*> deferredArgs;
	for (auto &arg : newExpr.Args())
	{
		if (&arg == newExpr.ClassName())
			continue;
		if (IsDeferredShape(arg))
			deferredArgs.push_back(&arg);
	}
	return deferredArgs;
}

//Failure-tail sweep of the deferred arguments: resolve them
//directly so the generic missing-target diagnostic still fires (the
//call's own diagnostics above stay). Idempotent — resolved arguments
//drop out. Exported through ExprResolver.h for the ctor paths' binding
//tails, which hold an already-collected list from their skip list.
void AcceptDeferredArgs(ISyntaxNodeVisitor &visitor,
	const std::vector<SnExpression*> &deferredArgs)
{
	for (auto *pArg : deferredArgs)
		if (!pArg->IsResolved())
			pArg->Accept(visitor);
}

//v0.13.13a D-2(a): the second arm that KNOWS its formal once selection
//completes — a lambda infers its expected type from the chosen ctor's
//formal, the same contract the Array(dims) arm established for brace
//literals. True when every deferred argument (named wrappers judged on
//their inner, mirroring CollectDeferredArgs) is an untyped lambda;
//brace and mixed shapes keep the gate closed (no other ctor formal
//shape can infer a brace target).
static bool DeferredArgsAreLambdasOnly(
	const std::vector<SnExpression*> &deferredArgs)
{
	if (deferredArgs.empty())
		return false;
	for (auto *pArg : deferredArgs)
	{
		SnExpression *pInner = pArg;
		if (pArg->Kind() == NK_NamedArgExpr)
			pInner = static_cast<SnNamedArgExpr&>(*pArg).Inner();
		if (!UntypedLambdaOf(*pInner))
			return false;
	}
	return true;
}

//0.13.10: the exported pair of the deferred-argument discipline for the
//paths that pre-resolve arguments in the caller scope (member-receiver
//and module-qualified calls): the skip-list for their eager pass, and
//the sweep their failure tails owe once the consumed member stops the
//walker. Both are idempotent — an argument the success channel already
//typed drops out of a later recompute.
void SweepDeferredInvokeArgs(ISyntaxNodeVisitor &visitor,
	SnInvokeExpr &invoke)
{
	auto deferredArgs = CollectDeferredArgs(invoke);
	AcceptDeferredArgs(visitor, deferredArgs);
}

//0.9.0: argument prologue of Access(SnInvokeExpr&) — collect the deferred
//bare brace arguments, resolve every other argument, and validate the
//call syntax. On either failure the sweep runs (the brace args still get
//their missing-target diagnostic) and the call stays unresolved.
bool ExprResolveAccessor::ResolveInvokeArguments(SnInvokeExpr &snInvoke,
	std::vector<SnExpression*> &deferredArgs)
{
	//0.9.0 (D11): a bare `{...}` argument has no type until an overload
	//is chosen — the chosen formal's type becomes its target. Keep
	//ResolveExpressionList off them and resolve them in
	//ResolveInvokeWithFunc; every exit that leaves the call unresolved
	//sweeps them through a direct Accept first.
	deferredArgs = CollectDeferredArgs(snInvoke);

	if (!ResolveExpressionList(snInvoke.Params(), &deferredArgs))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}

	//Phase 9c: caller-side syntax validation — independent of candidates.
	//Reports specific errors for structural issues that no overload can
	//satisfy (e.g. positional arg after named, duplicate named names).
	if (!ValidateInvokeSyntax(snInvoke))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}
	return true;
}

//Chosen-callee admission gates of Access(SnInvokeExpr&) — the Phase 12
//bare-method reject and friends. False = rejected with its diagnostic;
//the deferred-argument sweep has run and the call stays unresolved.
//v0.13.9 phase 4: the out-argument veto on virtual methods is gone —
//out actuals fail at their own retirement diagnostic first.
bool ExprResolveAccessor::AdmitChosenCallee(SnInvokeExpr &snInvoke,
	SnFunction *pCallee, const std::vector<FormalBinding> &bindings,
	const std::vector<SnExpression*> &deferredArgs)
{
	if (RejectBareMethodCall(snInvoke, pCallee))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}
	if (RejectTypeNameInstanceCall(snInvoke, pCallee))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}
	return true;
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	NLANG_ASSERT(!snInvoke.IsResolved());

	std::vector<SnExpression*> deferredArgs;
	if (!ResolveInvokeArguments(snInvoke, deferredArgs))
		return;

	//0.13.13a D-3: the postfix shape — the callee is a child expression,
	//not a name. The name-based channels below (delegate field lookup,
	//overload search by CalleeName) never apply: an empty CalleeName
	//would read as a not-found. The callee's own resolution decides the
	//binding.
	if (auto *pCalleeExpr = snInvoke.CalleeExpr())
	{
		ResolveCalleeExprInvoke(snInvoke, *pCalleeExpr, deferredArgs);
		return;
	}

	if (TryBindDelegateCall(snInvoke))
	{
		//A delegate call gates arguments against func signature slots, not
		//named formals — a deferred brace argument has no target to infer
		//from. The delegate gate already reported it; the sweep adds the
		//missing-target diagnostic and the call stays unresolved.
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}

	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	std::vector<SnFunction*> candidates;
	bool bNameMatchedImported = false;
	bool bVisibilityHintLogged = false;
	bool bAmbiguousCall = false;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings,
		bNameMatchedImported, bVisibilityHintLogged, &candidates,
		&bAmbiguousCall);

	if (!AdmitChosenCallee(snInvoke, pCallee, bindings, deferredArgs))
		return;

	if (res == FFR_FuncNameNotFound && RejectCrossNamespaceCall(snInvoke))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}

	if (res == FFR_ExactMatch || res == FFR_ApproximateMatch)
	{
		//M3b: the success tail is shared with the module-qualified call
		//path. A false return means an argument failed to bind (diagnostic
		//already logged) — leave the invoke unresolved.
		(void)ResolveInvokeWithFunc(snInvoke, *pCallee, res, bindings);
		return;
	}

	ResolveFailedInvoke(snInvoke, res, pCallee, bNameMatchedImported,
		bVisibilityHintLogged, candidates, deferredArgs,
		bAmbiguousCall);
}

//0.13.13a D-3: the postfix callee-expression shape. The callee resolves
//through the ordinary expression channels; its type decides the binding.
//Double-report discipline: a callee that failed to resolve already
//logged its own diagnostic — the invoke adds nothing. A resolved
//non-func-typed callee gets the not-callable report (a null
//EvalDataType means void — the established void-invoke convention).
//The successful binding runs against the callee's func instantiation
//through the shared delegate channel (BindDelegateInvokeAgainst) with a
//null value field: the postfix shape's codegen evaluates the callee
//expression itself, so there is no callee field to load.
void ExprResolveAccessor::ResolveCalleeExprInvoke(SnInvokeExpr &snInvoke,
	SnExpression &callee, std::vector<SnExpression*> &deferredArgs)
{
	if (!callee.IsResolved())
		callee.Accept(*m_pVisitor);
	if (!callee.IsResolved())
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}
	auto *pType = callee.EvalDataType();
	if (!pType || !IsFuncTypeDecl(pType))
	{
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"expression of type \"%s\" is not callable; a call "
			"requires a function-typed value.",
			pType ? pType->ToString().c_str() : "void");
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}
	BindDelegateInvokeAgainst(snInvoke, static_cast<SnClassDecl*>(pType),
		nullptr);
	//Delegate calls gate arguments against the func's signature slots,
	//not named formals — a deferred brace argument has no target to
	//infer from (the named delegate path's discipline, unchanged).
	AcceptDeferredArgs(*m_pVisitor, deferredArgs);
}

//The failure tail of Access(SnInvokeExpr). 0.9.0: an unresolved call
//whose name addresses a class is a constructor call — functions keep
//priority (the overload lookup already failed), and a foreign same-name
//function keeps the visibility hint as the complete diagnosis.
//Module import visibility (F20/M4): the "not visible here" hint is the
//complete diagnosis — appending the generic failure text would only
//blur it.
void ExprResolveAccessor::ResolveFailedInvoke(SnInvokeExpr &snInvoke,
	FindFuncResult res, SnFunction *pCallee, bool bNameMatchedImported,
	bool bVisibilityHintLogged, const std::vector<SnFunction*> &candidates,
	const std::vector<SnExpression*> &deferredArgs,
	bool bAmbiguousCall)
{
	if (bVisibilityHintLogged)
		return;
	//0.9.0: the ctor fallback binds arguments by count only — a deferred
	//brace argument has no formal to infer its target from, so the ctor
	//route must not consume the call (a resolved call with an unresolved
	//argument would reach the backend). The generic failure text plus the
	//sweep's missing-target diagnostic is the complete report.
	//v0.12.7 M1: the one arm that KNOWS its formal is Array(dims) — the
	//dims formal is int[], so the deferred literal there is inferable;
	//a successful inference re-opens the gate below. rbSwept marks that
	//the helper's Accepts already diagnosed the failures — the tail sweep
	//must not repeat them.
	bool bArgsAdmissible = deferredArgs.empty();
	bool bArgsSwept = false;
	if (!bArgsAdmissible && res == FFR_FuncNameNotFound)
	{
		bArgsAdmissible = TryInferArrayDimsDeferredArgs(snInvoke,
			deferredArgs, bArgsSwept);
		//v0.13.13a D-2(a): a lambda-only deferred set rides the ctor
		//route too — BindDeferredCtorArgs resolves it against the
		//selected ctor's formal (a failed selection sweeps it, so the
		//generic ctor text plus the lambda's own diagnostic stay the
		//complete report, mirroring the old LogInvokeFailure pair).
		//Pending function references never reach the deferred list
		//(they resolve as plain expressions); their gate is the empty
		//set above.
		if (!bArgsAdmissible)
			bArgsAdmissible = DeferredArgsAreLambdasOnly(deferredArgs);
	}
	if (res == FFR_FuncNameNotFound && bArgsAdmissible
		&& TryResolveCtorInvoke(snInvoke))
		return;
	LogInvokeFailure(snInvoke, res, pCallee, bNameMatchedImported,
		candidates, bAmbiguousCall);
	if (!bArgsSwept)
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
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

//The candidate collection and overload matching of the invoke paths
//(SearchFuncScope / SearchEnumScope / SearchSuperclassChain /
//CollectInvokeCandidates / FindFuncByInvoke / MatchInvokeAgainst) live
//in ExprResolverInvokeMatch.cpp (2026-10-12 source-size split). The
//func-ref argument binder BindArgFuncRefs joined ExprResolverBinding.cpp
//in the same spirit (v0.13.13a source-size split).

//M3b: the SUCCESS tail of Access(SnInvokeExpr), shared with the
//module-qualified call path so both bind a callee exactly alike.
bool ExprResolveAccessor::ResolveInvokeWithFunc(SnInvokeExpr &invoke,
	SnFunction &func, FindFuncResult match,
	std::vector<FormalBinding> &bindings)
{
	if ((match == FFR_ExactMatch || match == FFR_ApproximateMatch)
		&& !BindArgFuncRefs(bindings))
		return false;

	//0.9.0: deferred bare brace arguments bind now — the chosen formal's
	//declared type is the init list's target. A false return (diagnostic
	//logged) leaves the invoke unresolved, mirroring the func-ref path.
	if (!ResolveDeferredArgs(invoke, bindings))
		return false;

	//v0.13.9 phase 3 Task 5: the &-argument judgment (RefBindJudgment.cpp)
	//— the SAME judgment as a &-binding initializer, on raw argument
	//types (before the approximate cast fixup), with promotion marking.
	if (!::nlang::RejectRefBindCallArgs(m_Env, bindings))
		return false;

	if (match == FFR_ApproximateMatch)
		FixupParamTypesWithBindings(invoke, bindings);
	invoke.SetBindings(std::move(bindings));
	//A void return resolves to a null EvalDataType here — the established
	//void-invoke convention.
	ResolveFieldExprAs(invoke, &func);
	//0.13.1a Task 4 (D4): the template row's placeholder return types as
	//the inferred arguments at this call site — the value slot carries
	//the concrete type the caller sees (`identity(3)` is an Int32 here).
	if (func.IsGenericDeclaration() && !invoke.InferredTypeArgs().empty())
		SubstituteInvokeReturnType(invoke, func);
	BindArrayTypeToken(invoke);
	return true;
}

//The failure diagnostics of the invoke paths live in
//ExprResolverInvokeDiag.cpp (LogInvokeFailure and its MaybeLog* sweeps);
//the deferred-argument binding tail (ResolveDeferredArgs) lives with the
//other argument binders in ExprResolverBinding.cpp.

} //namespace nlang
