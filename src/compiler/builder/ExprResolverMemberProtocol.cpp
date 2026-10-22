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
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

namespace {

//0.9.10: the receiver chain is a bare type name (`D.equals(x)`) rather
//than a value — such a member call is a static call or nothing. Only
//class receivers reach this protocol arm (the caller's context gate),
//so the class-decl test is the complete receiver check.
bool IsTypeNameReceiver(SnMemberExpr &snMember)
{
	auto *pOuter = snMember.Outer();
	if (!pOuter || pOuter->Kind() != NK_IdentifierExpr)
		return false;
	auto *pField = static_cast<SnIdentifierExpr*>(pOuter)->Field();
	return pField && pField->Kind() == NK_ClassDecl;
}

//0.9.10 D2.4 #8: an enum TYPE NAME as receiver (`Color.toString()`) is
//not an enum value. The m_pContext arm in IsNonClassToStringReceiver
//cannot tell the two apart — both land on the enum declaration — so the
//receiver's bound field kind is the discriminator.
bool IsEnumTypeNameReceiver(SnMemberExpr &snMember)
{
	auto *pOuter = snMember.Outer();
	if (!pOuter || (pOuter->Kind() != NK_IdentifierExpr
		&& pOuter->Kind() != NK_MemberExpr))
		return false;
	auto *pField = static_cast<SnFieldExpr*>(pOuter)->Field();
	return pField && pField->Kind() == NK_EnumDecl;
}

//A same-name static anywhere on the class chain means the normal invoke
//path owns the call (it selects the overload or reports its own arity
//diagnostic). 0.9.10 D5: imported class stubs carry their exported
//public statics, so the walk continues through them; builtins keep
//stopping the walk (their Object-protocol members are intrinsic stubs,
//never user statics).
SnFunction *FindStaticMember(SnClassDecl *pClass, const std::string &name)
{
	std::set<SnClassDecl*> seen;
	for (auto *p = pClass; p && !seen.count(p); p = p->SuperClass())
	{
		seen.insert(p);
		if (p->IsBuiltinClass())
			break;
		for (auto &member : p->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& member.ContainFlags(NF_Static))
				return static_cast<SnFunction*>(&member);
	}
	return nullptr;
}

} //namespace

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
	//Bind m_pField (not just EvalDataType) so a chained member survives
	//IsDataExpr() — see BindTableStringMethodResult.
	auto* pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
	snMember.EvalDataType(pInt);
	snMember.m_pField = pInt;
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
	//0.9.10 D2.4: a type-name receiver (`D.equals(x)`) is a static
	//call site; a same-name static hands it to the normal invoke path.
	if (IsTypeNameReceiver(snMember))
	{
		if (FindStaticMember(pClassDecl, name))
			return false;
		return RejectTypeNameProtocolCall(invoke, name, pSavedContext);
	}
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
		//Bind m_pField (not just EvalDataType) so a chained member
		//(obj.toString().length()) survives IsDataExpr() — see
		//BindTableStringMethodResult.
		auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
		snMember.EvalDataType(pStr);
		snMember.m_pField = pStr;
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		m_pContext = pSavedContext;
		return true;
	}
	//Not consumed: keep the receiver context for the next phases' guards.
	return false;
}

bool ExprResolveAccessor::RejectTypeNameProtocolCall(SnInvokeExpr &invoke,
	const std::string &name, SyntaxNode *pSavedContext)
{
	m_Env.Log(CLL_Error, invoke.Location(),
		"the instance method \"%s\" must be called through a receiver "
		"object.",
		name.c_str());
	m_pContext = pSavedContext;
	return true;
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
	//Scalar via m_pContext (int x; x.toString(), 42.toString()).
	//0.7.5: registry-driven — bool (and the P4-P6 kinds) stringify
	//through the same OP_Prim_to_str path as int/float.
	if (!isNonClassToString && m_pContext
		&& ScalarPrimIndexOf(m_pContext->Kind()) >= 0)
	{
		isNonClassToString = true;
	}
	return isNonClassToString;
}

//Phase 8e-9b: non-class receiver toString() — enum, scalar, array.
//These types have no method table; the resolver accepts the call by setting
//EvalDataType=String + NF_Resolved. Codegen dispatches based on the
//outer expression's EvalDataType (enum→OP_Enum_to_str, scalar→OP_Prim_to_str,
//array→OP_Array_to_str). m_pField is bound to the same String type so a
//chained member (a.length().toString().length()) survives IsDataExpr();
//the type information otherwise flows through outer->EvalDataType(), same as
//struct/class/interface field access in codegen.
bool ExprResolveAccessor::TryResolveNonClassToString(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (pInnerExpr->Kind() != NK_InvokeExpr)
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	if (!(invoke.CalleeName() == "toString"
		&& invoke.Params().begin() == invoke.Params().end()))
		return false;
	//0.9.10 D2.4 #8: a type-name receiver is not an enum value — reject
	//before the non-class arm mistakes the declaration for the receiver.
	if (IsEnumTypeNameReceiver(snMember))
		return RejectTypeNameProtocolCall(invoke, invoke.CalleeName(),
			pSavedContext);
	if (!IsNonClassToStringReceiver(snMember))
		return false;
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	//Bind m_pField (not just EvalDataType) so a chained member
	//(a.length().toString().length()) survives IsDataExpr() — see
	//BindTableStringMethodResult.
	auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
	snMember.EvalDataType(pStr);
	snMember.m_pField = pStr;
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

} //namespace nlang
