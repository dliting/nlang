/*---
    ExprResolverMemberProtocol.cpp — 非类接收者的 toString 家族（enum / 标量 / 数组）。
    从 ExprResolver.cpp 抽取（2026-09-25 可维护性重构）。equals/getHashCode 与
    类接收者的 toString 协议臂随 Object 声明化退役（内建类真源化 Task 5）：
    三者成为声明继承的普通方法，由普通调用路径解析。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"

namespace nlang
{

namespace {

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

} //namespace

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
