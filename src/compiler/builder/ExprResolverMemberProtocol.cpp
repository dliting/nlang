/*---
    ExprResolverMemberProtocol.cpp — 对象协议方法阶段：equals / getHashCode（用户类）与 toString（非类接收者）。
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

//Equals/getHashCode arm shared by user classes: validation then the
//caller-scope resolve tail (int result).
bool ExprResolveAccessor::TryResolveUserClassEqualsOrGetHashCode(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
	const std::string &name, SyntaxNode *pSavedContext)
{
	//Round-12/14: same validation as the string branch — by-name
	//dispatch cannot bind named args, and the intrinsics read
	//exactly {this[, other]}.
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	if ((name == "equals" && ArgCountOf(invoke) != 1)
		|| (name == "getHashCode" && ArgCountOf(invoke) != 0))
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Built-in method '%s' called with the wrong number of arguments.",
			name.c_str());
		m_pContext = pSavedContext;
		return true;
	}
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

//Phase 8e-1: implicit Object protocol methods on user classes.
//Every user class inherits Equals(Object)→int and GetHashCode()→int from
//the synthesized Object base class. The methods have no AST representation
//(they're intrinsic stubs in VmBackend), so the normal InvokeExpr resolution
//path fails. Treat them as builtin virtuals here, parallel to stream methods.
//Args must be resolved in the CALLER's scope, not the empty Object scope —
//hence the early return before the inner Accept below.
bool ExprResolveAccessor::TryResolveObjectProtocolMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	auto* pClassDecl = static_cast<SnClassDecl*>(m_pContext);
	bool isUserClass = !pClassDecl->IsBuiltinClass();
	bool isObjectClass = pClassDecl->IsBuiltinClass()
		&& pClassDecl->Name() == "Object";
	if ((name == "equals" || name == "getHashCode") && isUserClass)
		return TryResolveUserClassEqualsOrGetHashCode(snMember, pInnerExpr,
			invoke, name, pSavedContext);
	//Phase 8e-9b: string toString() — user class inherits Object.toString().
	//User-defined override is resolved via the normal class-method path
	//(CalleeName resolves to a real SnFunction); this branch only catches
	//the no-override case to fall through to Object intrinsic dispatch.
	if (name == "toString"
		&& invoke.Params().begin() == invoke.Params().end()
		&& (isUserClass || isObjectClass))
	{
		m_pContext = pSavedContext;
		RemoveFlags(ERF_SearchInParentOnly);
		ResolveExpressionList(invoke.Params());
		pInnerExpr->AddFlags(NF_Resolved);
		snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		m_pContext = pSavedContext;
		return true;
	}
	//Not consumed: keep the receiver context for the next phases' guards.
	return false;
}

//Phase 8e-9b: non-class toString receiver detection — enum, int, float,
//array. Detection: m_pContext (the receiver's type context) is NK_EnumDecl,
//NK_Int32, or NK_Float. For enum literal access (Color.Green.toString()),
//m_pContext is NK_Int32 (SnEnumMember::EvalDataType returns NK_Int32), so
//we also check the outer's Field() chain for NK_EnumMember.
bool ExprResolveAccessor::IsNonClassToStringReceiver(SnMemberExpr &snMember)
{
	bool isNonClassToString = false;
	//Array receiver — Array is a VM primitive, not a class, so
	//this is the only arm that catches it: the receiver's type
	//context is the interned array token (never a scalar), so
	//the enum and int/float arms below cannot fire. Detection
	//keys on the FIELD's declared array-ness for identifier/
	//member shapes (the same lvalue family as the receiver
	//gate above) — the authoritative declaration signal.
	{
		auto outerKind = snMember.Outer()->Kind();
		if (outerKind == NK_IdentifierExpr
			|| outerKind == NK_MemberExpr)
		{
			auto& outerFieldExpr = static_cast<SnFieldExpr&>(
				*snMember.Outer());
			auto* outerField = outerFieldExpr.Field();
			if (outerField && outerField->IsArrayType())
				isNonClassToString = true;
		}
	}
	//Enum via m_pContext (typed enum variable: Color c; c.toString())
	if (!isNonClassToString && m_pContext
		&& m_pContext->Kind() == NK_EnumDecl)
		isNonClassToString = true;
	//Enum via outer Field() chain (Color.Green.toString())
	if (!isNonClassToString)
	{
		auto outerKind = snMember.Outer()->Kind();
		if (outerKind == NK_MemberExpr || outerKind == NK_IdentifierExpr)
		{
			auto& outerFieldExpr = static_cast<SnFieldExpr&>(
				*snMember.Outer());
			auto* outerField = outerFieldExpr.Field();
			if (outerField && outerField->Kind() == NK_EnumMember)
				isNonClassToString = true;
		}
	}
	//Int/float via m_pContext (int x; x.toString(), 42.toString())
	if (!isNonClassToString && m_pContext
		&& (m_pContext->Kind() == NK_Int32
			|| m_pContext->Kind() == NK_Float))
	{
		isNonClassToString = true;
	}
	return isNonClassToString;
}

//Phase 8e-9b: non-class receiver toString() — enum, int, float, array.
//These types have no method table; the resolver accepts the call by setting
//EvalDataType=String + NF_Resolved. Codegen dispatches based on the
//outer expression's EvalDataType (enum→OP_Enum_to_str, int→OP_Int32_to_str,
//float→OP_Float_to_str). No m_pField hack needed — the type information
//flows through the existing outer->EvalDataType() channel, same as struct/
//class/interface field access in codegen.
bool ExprResolveAccessor::TryResolveNonClassToString(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (pInnerExpr->Kind() != NK_InvokeExpr)
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	if (!(invoke.CalleeName() == "toString"
		&& invoke.Params().begin() == invoke.Params().end()))
		return false;
	if (!IsNonClassToStringReceiver(snMember))
		return false;
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

} //namespace nlang
