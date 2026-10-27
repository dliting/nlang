/*---
    ExprResolverMember.cpp — 成员表达式解析编排：Access(SnMemberExpr&) 阶段序 +头部拦截 + 收尾绑定。
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

//Phase 9e (pre-existing gap exposed by out params): a method invoke's
//ARGUMENTS must resolve in the caller's scope. m_pContext is the
//receiver's class here (set for the callee lookup) and
//ERF_SearchInParentOnly hides the calling function's locals — so
//`c.f(v)` failed with "Cannot resolve the field: v". Existing tests
//never hit this because they only pass literals. Resolve the args in
//the caller scope first (mirroring the stream/generic early-return
//paths above), then re-enter the class scope so Access(SnInvokeExpr)
//finds the callee; its ResolveExpressionList skips resolved params.
void ExprResolveAccessor::ResolveInvokeArgsInCallerScope(
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		auto* pClassCtx = m_pContext;
		m_pContext = pSavedContext;
		RemoveFlags(ERF_SearchInParentOnly);
		ResolveExpressionList(invoke.Params());
		m_pContext = pClassCtx;
		AddFlags(ERF_SearchInParentOnly);
	}
}

//Tail of the normal (non-builtin) path: the inner expression resolved.
//Delegate invokes keep the invoke's own return type (ResolveFieldExprAs
//would re-type the member as the delegate VALUE's declared type).
void ExprResolveAccessor::FinishResolvedMember(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr)
{
	//Phase 13 Step 2: a delegate member invoke (obj.cb(x)) resolved
	//the invoke against the FIELD's func signature — the member's type
	//is the invoke's own return type. ResolveFieldExprAs would instead
	//re-type the member as the delegate VALUE's declared type
	//(func<...>), masking the call result at every consumer.
	if (pInnerExpr->Kind() == NK_InvokeExpr
		&& pInnerExpr->Field()
		&& pInnerExpr->Field()->Kind() != NK_Function)
	{
		snMember.m_pField = pInnerExpr->Field();
		if (pInnerExpr->EvalDataType())
			snMember.EvalDataType(pInnerExpr->EvalDataType());
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
	}
	else
	{
		ResolveFieldExprAs(snMember, pInnerExpr->Field());
		//Plain member-field path: the shared helper above sets NF_Resolved internally.
		BindArrayTypeToken(snMember);
	}
}

//Round-12/14 shared rejection for the builtin by-name method families:
//by-name dispatch cannot bind named arguments, and intrinsics return
//through pResult only — out arguments can never write back.
bool ExprResolveAccessor::RejectNamedOrOutArguments(SnInvokeExpr &invoke)
{
	if (HasNamedArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Named arguments are not supported by built-in methods.");
		return true;
	}
	if (HasOutArgument(invoke))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"out arguments are not supported by built-in methods.");
		return true;
	}
	return false;
}

//Receiver-scope switch: a data-typed outer resolves in its type context;
//a type outer (namespace/class name) resolves in the type's scope, with
//the enum-member re-anchor for `Color.Blue.rank()` receivers (the member
//masquerades as Int32, which would land the context on the int builtin).
void ExprResolveAccessor::SwitchContextToReceiver(SnMemberExpr &snMember)
{
	if (snMember.Outer()->IsDataExpr())
		m_pContext = snMember.Outer()->EvalDataType();
	else
	{
		auto &outerFieldExpr = static_cast<SnFieldExpr &>(*snMember.Outer());
		auto* pOuterField = static_cast<SnField *>(outerFieldExpr.Field());
		m_pContext = pOuterField;
		//A namespace receiver must stay on the namespace node so a non-function
		//member (enum value, constant, nested type: `shapes.Color.Green`) is
		//looked up inside it. SnNamespace carries no NF_Type, so the generic
		//IsTypeField() test below is false and would switch to its
		//EvalDataType() — the meta SnType singleton, which has none of the
		//namespace's members. Function calls (`ns.f(...)`) never reach here:
		//TryResolveModuleQualified consumes them at the chain head.
		if (pOuterField && pOuterField->Kind() == NK_Namespace)
			m_pContext = pOuterField;
		else if (pOuterField && !pOuterField->IsTypeField())
			m_pContext = snMember.Outer()->EvalDataType();
		//Phase 12: enum member receiver (`Color.Blue.rank()`). The member
		//masquerades as Int32 (SnEnumMember::EvalDataType), which would
		//land the context on the int builtin — re-anchor to the owning
		//enum decl so the method search starts at the enum scope.
		if (pOuterField && pOuterField->Kind() == NK_EnumMember)
			m_pContext = pOuterField->Parent();
	}
}

//Array-valued receivers expose no user methods — the gate gives the
//named, actionable rejection ("index an element first") instead of
//the generic resolution failure a token-typed receiver produces.
//Two builtin exemptions fall through below (see the inline checks):
//length() — the array's one builtin method, any arity (the builtin
//dispatch owns the arity error) — and toString for lvalue receivers
//(identifier / member field): the non-class toString dispatch below
//detects exactly those shapes via the IsArrayType() field check.
//Call-result array values (l.get(0), obj.mk(), l[0], delegate calls)
//route nowhere in that dispatch — their string conversion is the cast
//table's array→string coercion in expression positions, not a method
//call.
//(Pre-token this was worse: the element masquerade bound the
//ELEMENT type's method table — string[] receivers entered the
//string-builtin block, enum/class receivers bound user methods —
//and codegen passed the array's heap index as the receiver; verified
//enum[].rank() returned heapIdx+10. Phase 12 review MAJOR-1, trap-12
//family instance #7.)
bool ExprResolveAccessor::RejectArrayReceiverMethodCall(
	SnMemberExpr &snMember, SnExpression *pOuterExpr,
	SyntaxNode *pSavedContext)
{
	auto* pInnerForGate = snMember.Inner();
	if (pInnerForGate && pInnerForGate->Kind() == NK_InvokeExpr
		&& pOuterExpr->IsArrayValued())
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerForGate);
		//length() is the array's one builtin method — it always falls
		//through to the builtin dispatch in Access(), which owns the
		//arity check (a dedicated "takes no arguments" message) instead
		//of this generic reject.
		if (invoke.CalleeName() == "length"
			|| invoke.CalleeName() == "rank"
			|| invoke.CalleeName() == "shape")
			return false;
		if (invoke.CalleeName() != "toString"
			|| !(pOuterExpr->IsArrayValued()
				&& IsPlainLvalueShape(*pOuterExpr)))
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"methods cannot be called on an array; index an element "
				"first (e.g. a[i].%s(...)).",
				invoke.CalleeName().c_str());
			m_pContext = pSavedContext;
			return true;
		}
	}
	return false;
}

//Void-support: a receiver that resolved to a call result with no
//type leaves no context to search — the dispatches in Access would
//dereference the null. Name the void result (the assign arm's wording
//family) and consume the member so the chain cannot re-report. True =
//rejected (already diagnosed).
bool ExprResolveAccessor::RejectVoidReceiverMemberAccess(
	SnMemberExpr &snMember, SyntaxNode *pSavedContext)
{
	if (m_pContext)
		return false;
	m_Env.Log(CLL_Error, snMember.Location(),
		"cannot access a member of the result of void function \"%s\".",
		snMember.Outer()->ToString().c_str());
	FinishConsumedMember(snMember);
	m_pContext = pSavedContext;
	return true;
}

//Chain-head resolution: the library namespace interception, the module-table
//fallback, and the outer expression's own resolution. Returns true when
//the member is consumed (resolved or diagnosed).
bool ExprResolveAccessor::TryResolveMemberHead(SnMemberExpr &snMember,
	SnExpression *pOuterExpr)
{
	//Module import visibility (spec §6.2 rule 5): module-table fallback
	//K3 thread-package materialization: the typed-spawn narrow arm rides
	//the same head position, ahead of the module-qualified fallback, so
	//its diagnostics never double. Every other thread.* shape declines
	//to the fallback — thread is an ordinary stdlib package now. m12
	//priority lives inside both helpers via the same probes — a user
	//local/field/type named `thread` wins.
	if (TryResolveThreadSpawn(snMember))
		return true;
	//for dotted call chains. Runs BEFORE the outer resolves — a module
	//diagnostic must not double with a spurious "Cannot resolve the field",
	//and a declined chain leaves normal resolution untouched.
	if (TryResolveModuleQualified(snMember))
		return true;
	//v2.2: `lib.funcName` in a value position — the pending function
	//reference (shared channel with bound method references). Non-function
	//tips decline to the package-value path below.
	if (TryResolveModuleFuncValue(snMember))
		return true;
	//Phase 5: the package-qualified value chain (`alib.Color.Green`) —
	//same position, same consumption contract (declines leave the generic
	//path untouched).
	if (TryResolveQualifiedTypeValue(snMember))
		return true;
	//0.9.12 D4: the module-qualified type reference (`lib.Mix`) — binds
	//the type declaration so the generic path continues in the type scope.
	if (TryResolveModuleTypeRef(snMember))
		return true;
	pOuterExpr->Accept(*m_pVisitor);
	if (!pOuterExpr->IsResolved())
	{
		//spec §6.2 last line: the outer chain died as a whole (its head
		//resolved as nothing, or a shadowing class failed mid-chain) —
		//note a module path sharing the dotted name, if one exists.
		MaybeLogModuleHint(JoinDots(OuterIdentifierChain(snMember)));
		return true;
	}
	return false;
}

//Access's builtin by-name method families, in the historical dispatch
//order (each family logs its own rejections; the string family runs
//before the stream family — close() is a stream method name too).
bool ExprResolveAccessor::TryResolveBuiltinMethodFamilies(
	SnMemberExpr &snMember, SnExpression *pOuterExpr,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (TryResolveStringBuiltinMethod(snMember, pInnerExpr, pSavedContext))
		return true;
	if (TryResolveArrayLengthMethod(snMember, pOuterExpr, pInnerExpr,
		pSavedContext))
		return true;
	if (TryResolveStreamBuiltinMethod(snMember, pInnerExpr, pSavedContext))
		return true;
	if (TryResolveNonClassToString(snMember, pInnerExpr, pSavedContext))
		return true;
	if (TryResolveGenericContainerMethod(snMember, pInnerExpr,
		pSavedContext))
		return true;
	return false;
}

void ExprResolveAccessor::Access(SnMemberExpr &snMember)
{
	assert(!snMember.IsResolved());

	auto pOuterExpr = snMember.Outer();
	assert(pOuterExpr);

	if (TryResolveMemberHead(snMember, pOuterExpr))
		return;

	auto pSavedContext = m_pContext;
	SwitchContextToReceiver(snMember);

	if (RejectVoidReceiverMemberAccess(snMember, pSavedContext))
		return;
	if (RejectArrayReceiverMethodCall(snMember, pOuterExpr, pSavedContext))
		return;

	SCOPED_FLAG_RESETER(*this);
	AddFlags(ERF_SearchInParentOnly);
	auto pInnerExpr = snMember.Inner();
	assert(pInnerExpr);

	//Builtin by-name method families, in the historical dispatch order
	//(each family logs its own rejections).
	if (TryResolveBuiltinMethodFamilies(snMember, pOuterExpr, pInnerExpr,
		pSavedContext))
		return;

	ResolveInvokeArgsInCallerScope(pInnerExpr, pSavedContext);

	pInnerExpr->Accept(*m_pVisitor);
	if (pInnerExpr->IsResolved())
		FinishResolvedMember(snMember, pInnerExpr);

	m_pContext = pSavedContext;
}

} //namespace nlang
