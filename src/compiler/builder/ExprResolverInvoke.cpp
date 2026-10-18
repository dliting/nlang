/*---
    ExprResolverInvoke.cpp — 调用解析（参数前置、成功尾、失败诊断）
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
    2026-10-12 source-size 拆分：候选收集与重载匹配迁入
    ExprResolverInvokeMatch.cpp。
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
//is a frame-shift bug — codegen's bare-invoke path emits OP_CallFunc
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
	//emits OP_CallFunc slotBase 0 against a frame with no __this slot.
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

//0.9.0: the bare brace arguments of this call — untyped init lists whose
//target type can only be the chosen overload's formal. Empty for every
//other argument shape.
static std::vector<SnExpression*> CollectDeferredBraceArgs(
	SnInvokeExpr &invoke)
{
	std::vector<SnExpression*> deferredArgs;
	for (auto &param : invoke.Params())
		if (UntypedInitListOf(param))
			deferredArgs.push_back(&param);
	return deferredArgs;
}

//Failure-tail sweep of the deferred brace arguments: resolve them
//directly so the generic missing-target diagnostic still fires (the
//call's own diagnostics above stay).
static void AcceptDeferredBraceArgs(ISyntaxNodeVisitor &visitor,
	const std::vector<SnExpression*> &deferredArgs)
{
	for (auto *pArg : deferredArgs)
		if (!pArg->IsResolved())
			pArg->Accept(visitor);
}

//0.9.0: argument prologue of Access(SnInvokeExpr&) — collect the deferred
//bare brace arguments, resolve every other argument, and validate the
//call syntax. On either failure the sweep runs (the brace args still get
//their missing-target diagnostic) and the call stays unresolved.
bool ExprResolveAccessor::ResolveInvokeArguments(SnInvokeExpr &snInvoke,
	std::vector<SnExpression*> &deferredBraceArgs)
{
	//0.9.0 (D11): a bare `{...}` argument has no type until an overload
	//is chosen — the chosen formal's type becomes its target. Keep
	//ResolveExpressionList off them and resolve them in
	//ResolveInvokeWithFunc; every exit that leaves the call unresolved
	//sweeps them through a direct Accept first.
	deferredBraceArgs = CollectDeferredBraceArgs(snInvoke);

	if (!ResolveExpressionList(snInvoke.Params(), &deferredBraceArgs))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return false;
	}

	//Phase 9c: caller-side syntax validation — independent of candidates.
	//Reports specific errors for structural issues that no overload can
	//satisfy (e.g. positional arg after named, duplicate named names).
	if (!ValidateInvokeSyntax(snInvoke))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return false;
	}
	return true;
}

//Chosen-callee admission gates of Access(SnInvokeExpr&) — the Phase 9e
//out-argument veto on virtual (by-name dispatched) methods and the
//Phase 12 bare-method reject. False = rejected with its diagnostic; the
//deferred-argument sweep has run and the call stays unresolved.
bool ExprResolveAccessor::AdmitChosenCallee(SnInvokeExpr &snInvoke,
	SnFunction *pCallee, const std::vector<FormalBinding> &bindings,
	const std::vector<SnExpression*> &deferredBraceArgs)
{
	//Phase 9e: out arguments on virtual (by-name dispatched) methods are
	//rejected before anything binds — see OutArgOnDispatchedCalleeRejected.
	if (pCallee
		&& OutArgOnDispatchedCalleeRejected(snInvoke, *pCallee, bindings))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return false;
	}
	if (RejectBareMethodCall(snInvoke, pCallee))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return false;
	}
	if (RejectTypeNameInstanceCall(snInvoke, pCallee))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return false;
	}
	return true;
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	assert(!snInvoke.IsResolved());

	std::vector<SnExpression*> deferredBraceArgs;
	if (!ResolveInvokeArguments(snInvoke, deferredBraceArgs))
		return;

	if (TryBindDelegateCall(snInvoke))
	{
		//A delegate call gates arguments against func signature slots, not
		//named formals — a deferred brace argument has no target to infer
		//from. The delegate gate already reported it; the sweep adds the
		//missing-target diagnostic and the call stays unresolved.
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
		return;
	}

	SnFunction *pCallee;
	std::vector<FormalBinding> bindings;
	std::vector<SnFunction*> candidates;
	bool bNameMatchedImported = false;
	bool bVisibilityHintLogged = false;
	auto res = FindFuncByInvoke(pCallee, snInvoke, bindings,
		bNameMatchedImported, bVisibilityHintLogged, &candidates);

	if (!AdmitChosenCallee(snInvoke, pCallee, bindings, deferredBraceArgs))
		return;

	if (res == FFR_FuncNameNotFound && RejectCrossNamespaceCall(snInvoke))
	{
		AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
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
		bVisibilityHintLogged, candidates, deferredBraceArgs);
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
	const std::vector<SnExpression*> &deferredBraceArgs)
{
	if (bVisibilityHintLogged)
		return;
	//0.9.0: the ctor fallback binds arguments by count only — a deferred
	//brace argument has no formal to infer its target from, so the ctor
	//route must not consume the call (a resolved call with an unresolved
	//argument would reach the backend). The generic failure text plus the
	//sweep's missing-target diagnostic is the complete report.
	if (res == FFR_FuncNameNotFound && deferredBraceArgs.empty()
		&& TryResolveCtorInvoke(snInvoke))
		return;
	LogInvokeFailure(snInvoke, res, pCallee, bNameMatchedImported,
		candidates);
	AcceptDeferredBraceArgs(*m_pVisitor, deferredBraceArgs);
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
//in ExprResolverInvokeMatch.cpp (2026-10-12 source-size split).

//M3b: the SUCCESS tail of Access(SnInvokeExpr), shared with the
//module-qualified call path so both bind a callee exactly alike.
bool ExprResolveAccessor::ResolveInvokeWithFunc(SnInvokeExpr &invoke,
	SnFunction &func, FindFuncResult match,
	std::vector<FormalBinding> &bindings)
{
	//Phase 13: argument-position function references bind against the
	//formal func types of the chosen overload (ComputeBindingDistance
	//already required an exact signature match for candidacy). Step 2
	//adds the receiver-bound member form (c.foo).
	if (match == FFR_ExactMatch || match == FFR_ApproximateMatch)
	{
		for (auto &b : bindings)
		{
			if (b.kind == FormalBinding::B_Default || !b.pCallerExpr)
				continue;
			if (IsUnboundFuncRef(*b.pCallerExpr))
			{
				if (!BindFuncRefToExpected(m_Env,
					*static_cast<SnIdentifierExpr*>(b.pCallerExpr),
					b.pFormal->EvalDataType()))
					return false;
			}
			else if (IsUnboundMemberFuncRef(*b.pCallerExpr)
				&& !BindMemberFuncRefToExpected(m_Env,
					*static_cast<SnMemberExpr*>(b.pCallerExpr),
					b.pFormal->EvalDataType()))
				return false;
		}
	}

	//0.9.0: deferred bare brace arguments bind now — the chosen formal's
	//declared type is the init list's target. A false return (diagnostic
	//logged) leaves the invoke unresolved, mirroring the func-ref path.
	if (!ResolveDeferredBraceArgs(bindings))
		return false;

	if (match == FFR_ApproximateMatch)
		FixupParamTypesWithBindings(invoke, bindings);
	invoke.SetBindings(std::move(bindings));
	//A void return resolves to a null EvalDataType here — the established
	//void-invoke convention.
	ResolveFieldExprAs(invoke, &func);
	BindArrayTypeToken(invoke);
	return true;
}

//0.9.0: the deferred bare brace arguments of the chosen overload — the
//formal's declared type is the init list's target (the same declaration
//field shape the assignment site passes as InferredTarget). False when
//any of them failed to resolve; its diagnostic is already logged.
bool ExprResolveAccessor::ResolveDeferredBraceArgs(
	std::vector<FormalBinding> &bindings)
{
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default || !b.pCallerExpr)
			continue;
		auto *pInitList = UntypedInitListOf(*b.pCallerExpr);
		if (!pInitList)
			continue;
		pInitList->InferredTarget(b.pFormal);
		b.pCallerExpr->Accept(*m_pVisitor);
		if (!b.pCallerExpr->IsResolved())
			return false;
	}
	return true;
}

//Phase 9e: out arguments on virtual methods are rejected — the
//writeback mask is baked into the call instruction against the
//static callee's parameter layout; a runtime override resolved by
//name-based dispatch could disagree with it. Interface methods are
//always dispatched by name (never NF_Virtual-flagged), so they are
//covered via the parent decl kind. Shared by the bare and the
//module-qualified call paths.
bool ExprResolveAccessor::OutArgOnDispatchedCalleeRejected(
	const SnInvokeExpr &invoke, const SnFunction &callee,
	const std::vector<FormalBinding> &bindings) const
{
	bool bDispatchedByName = callee.ContainFlags(NF_Virtual)
		|| (callee.Parent()
			&& callee.Parent()->Kind() == NK_InterfaceDecl);
	if (!bDispatchedByName)
		return false;
	for (auto &b : bindings)
	{
		if (b.bIsOut)
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"out arguments are not supported on virtual method "
				"\"%s\".",
				callee.Name().c_str());
			return true;
		}
	}
	return false;
}

//The failure diagnostics of the invoke paths live in
//ExprResolverInvokeDiag.cpp (LogInvokeFailure and its MaybeLog* sweeps).

} //namespace nlang
