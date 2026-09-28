/*---
    ExprResolverMember.cpp — 成员表达式解析编排：Access(SnMemberExpr&) 阶段序 +头部拦截 + 异常字段 + 收尾绑定。
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

//Phase 9d: built-in Exception class field access (e.message, e.backtrace).
//The synthetic SnClassDecl has no real member fields, so resolve by name.
//Field offsets are hard-coded in VmBackend::FindClassFieldOffset:
//  message  → slot[1] (offset 4)
//  backtrace → slot[2] (offset 8)
//This also handles user subclasses of Exception — walk SuperClass()
//chain to detect Exception ancestry.
bool ExprResolveAccessor::TryResolveExceptionField(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& pInnerExpr->Kind() == NK_IdentifierExpr))
		return false;
	auto* pClass = static_cast<SnClassDecl*>(m_pContext);
	//Walk SuperClass chain looking for a built-in Exception class.
	bool isExceptionSubclass = false;
	for (SnClassDecl* pWalk = pClass; pWalk; ) {
		if (pWalk->IsBuiltinClass()
			&& IsBuiltinExceptionClassName(pWalk->Name())) {
			isExceptionSubclass = true;
			break;
		}
		pWalk = pWalk->SuperClass();
	}
	if (!isExceptionSubclass)
		return false;
	auto& innerId = static_cast<SnIdentifierExpr&>(*pInnerExpr);
	const auto& fieldName = innerId.Name();
	SnField* pResultField = nullptr;
	if (fieldName == "message") {
		pResultField = SnBuiltinDataType::InstanceOf(NK_String);
	} else if (fieldName == "backtrace") {
		//backtrace is List<string> — synthesize the generic instantiation.
		auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
		std::vector<SnField*> listArgs{ pStr };
		pResultField = GetGenericClassDecl("List", listArgs, {},
			pInnerExpr->Location());
	}
	if (!pResultField)
		return false;
	innerId.AddFlags(NF_Resolved);
	snMember.EvalDataType(pResultField);
	snMember.m_pField = pResultField;
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

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
	//the invoke against the FIELD's Func signature — the member's type
	//is the invoke's own return type. ResolveFieldExprAs would instead
	//re-type the member as the delegate VALUE's declared type
	//(Func<...>), masking the call result at every consumer.
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
		if (pOuterField && !pOuterField->IsTypeField())
			m_pContext = snMember.Outer()->EvalDataType();
		//Phase 12: enum member receiver (`Color.Blue.rank()`). The member
		//masquerades as Int32 (SnEnumMember::EvalDataType), which would
		//land the context on the int builtin — re-anchor to the owning
		//enum decl so the method search starts at the enum scope.
		if (pOuterField && pOuterField->Kind() == NK_EnumMember)
			m_pContext = pOuterField->Parent();
	}
}

//Array-valued receivers expose no methods — the gate gives the
//named, actionable rejection ("index an element first") instead of
//the generic resolution failure a token-typed receiver produces.
//(Pre-token this was worse: the element masquerade bound the
//ELEMENT type's method table — string[] receivers entered the
//string-builtin block, enum/class receivers bound user methods —
//and codegen passed the array's heap index as the receiver; verified
//enum[].rank() returned heapIdx+10. Phase 12 review MAJOR-1, trap-12
//family instance #7.)
//toString is exempt ONLY for lvalue receivers (identifier / member
//field): the non-class toString dispatch below detects exactly those
//shapes via the IsArrayType() field check. Call-result array values
//(l.get(0), obj.mk(), l[0], delegate calls) route nowhere in that
//dispatch — their string conversion is the cast table's array→string
//coercion in expression positions, not a method call.
bool ExprResolveAccessor::RejectArrayReceiverMethodCall(
	SnMemberExpr &snMember, SnExpression *pOuterExpr,
	SyntaxNode *pSavedContext)
{
	auto* pInnerForGate = snMember.Inner();
	if (pInnerForGate && pInnerForGate->Kind() == NK_InvokeExpr
		&& pOuterExpr->IsArrayValued())
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerForGate);
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

//Chain-head resolution: the stdlib namespace interception, the module-table
//fallback, and the outer expression's own resolution. Returns true when
//the member is consumed (resolved or diagnosed).
bool ExprResolveAccessor::TryResolveMemberHead(SnMemberExpr &snMember,
	SnExpression *pOuterExpr)
{
	//Module import visibility (spec §6.2 rule 5): module-table fallback
	//for dotted call chains. Runs BEFORE the outer resolves — a module
	//diagnostic must not double with a spurious "Cannot resolve the field",
	//and a declined chain leaves normal resolution untouched.
	if (TryResolveModuleQualified(snMember))
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

void ExprResolveAccessor::Access(SnMemberExpr &snMember)
{
	assert(!snMember.IsResolved());

	auto pOuterExpr = snMember.Outer();
	assert(pOuterExpr);

	if (TryResolveMemberHead(snMember, pOuterExpr))
		return;

	auto pSavedContext = m_pContext;
	SwitchContextToReceiver(snMember);

	if (RejectArrayReceiverMethodCall(snMember, pOuterExpr, pSavedContext))
		return;

	SCOPED_FLAG_RESETER(*this);
	AddFlags(ERF_SearchInParentOnly);
	auto pInnerExpr = snMember.Inner();
	assert(pInnerExpr);

	//Builtin by-name method families, in the historical dispatch order
	//(each phase logs its own rejections).
	if (TryResolveStringBuiltinMethod(snMember, pInnerExpr, pSavedContext))
		return;
	if (TryResolveArrayLengthProperty(snMember, pOuterExpr, pInnerExpr,
		pSavedContext))
		return;
	if (TryResolveStreamBuiltinMethod(snMember, pInnerExpr, pSavedContext))
		return;
	if (TryResolveObjectProtocolMethod(snMember, pInnerExpr, pSavedContext))
		return;
	if (TryResolveNonClassToString(snMember, pInnerExpr, pSavedContext))
		return;
	if (TryResolveGenericContainerMethod(snMember, pInnerExpr,
		pSavedContext))
		return;
	if (TryResolveExceptionField(snMember, pInnerExpr, pSavedContext))
		return;

	ResolveInvokeArgsInCallerScope(pInnerExpr, pSavedContext);

	pInnerExpr->Accept(*m_pVisitor);
	if (pInnerExpr->IsResolved())
		FinishResolvedMember(snMember, pInnerExpr);

	m_pContext = pSavedContext;
}

} //namespace nlang
