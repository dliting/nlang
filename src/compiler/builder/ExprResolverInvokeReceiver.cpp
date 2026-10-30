/*---
    ExprResolverInvokeReceiver.cpp — 调用头接收者分类与命名空间过滤
    （0.9.10 D2.4 家族）。从 ExprResolverInvokeMatch.cpp 抽取
    （2026-10-30 source-size 拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <set>

namespace nlang
{

namespace
{

//0.9.10 D2.4: the member expr whose inner identifier is idExpr (the
//receiver.name value shape), or null for every other position.
SnMemberExpr *AsMemberInner(SnIdentifierExpr &idExpr)
{
	auto *pParent = idExpr.Parent();
	if (pParent && pParent->Kind() == NK_MemberExpr
		&& static_cast<SnMemberExpr*>(pParent)->Inner() == &idExpr)
		return static_cast<SnMemberExpr*>(pParent);
	return nullptr;
}

//A namespace qualifier is not a receiver — module-level function
//references keep their own channel, unfiltered.
bool OuterIsNamespaceQualifier(SnExpression *pOuter)
{
	if (pOuter && (pOuter->Kind() == NK_IdentifierExpr
		|| pOuter->Kind() == NK_MemberExpr))
	{
		auto *pOuterField = static_cast<SnFieldExpr*>(pOuter)->Field();
		return pOuterField && pOuterField->Kind() == NK_Namespace;
	}
	return false;
}

} //namespace

//0.9.10 D2.4: a receiver expression that names a type (class /
//interface / enum) — that form selects the static namespace.
//Supported spelling is a bare identifier (spec D2.1); a
//package-qualified type receiver (`lib.Util.f(...)`) does not resolve
//and stays a documented limitation.
bool ReceiverNamesType(SnExpression *pOuter)
{
	if (!pOuter)
		return false;
	if (pOuter->Kind() != NK_IdentifierExpr
		&& pOuter->Kind() != NK_MemberExpr)
		return false;
	auto *pField = static_cast<SnFieldExpr*>(pOuter)->Field();
	return pField && (pField->Kind() == NK_ClassDecl
		|| pField->Kind() == NK_InterfaceDecl
		|| pField->Kind() == NK_EnumDecl);
}

//0.9.10 D2.4: first same-name member of ONE namespace (static vs
//instance) along the receiver's chain — the rebind and cross-namespace
//diagnostic search. 0.9.10 D5: imported class stubs carry their
//exported public statics, so the walk continues through them; builtins
//still stop the walk (their members are intrinsic stubs, never user
//statics). The enum and interface arms scan their own members only.
SnFunction *FindMethodInNamespace(SyntaxNode *pContext,
	const std::string &name, bool bStatic)
{
	if (pContext && pContext->Kind() == NK_EnumDecl)
	{
		for (auto &method : static_cast<SnEnumDecl*>(pContext)->Methods())
			if (method.Name() == name
				&& method.ContainFlags(NF_Static) == bStatic)
				return &method;
		return nullptr;
	}
	if (pContext && pContext->Kind() == NK_InterfaceDecl)
	{
		for (auto &member : static_cast<SnInterfaceDecl*>(pContext)->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& member.ContainFlags(NF_Static) == bStatic)
				return static_cast<SnFunction*>(&member);
		return nullptr;
	}
	if (!(pContext && pContext->Kind() == NK_ClassDecl))
		return nullptr;
	std::set<SnClassDecl*> seen;
	for (auto *p = static_cast<SnClassDecl*>(pContext); p && !seen.count(p);
		p = p->SuperClass())
	{
		seen.insert(p);
		if (p->IsBuiltinClass())
			break;
		for (auto &member : p->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& member.ContainFlags(NF_Static) == bStatic)
				return static_cast<SnFunction*>(&member);
	}
	return nullptr;
}

//0.9.10 D2.4: the member VALUE channel (receiver.name with no invoke)
//gets the same namespace split as calls. Called from
//Access(SnIdentifierExpr) once the plain search bound the inner name —
//that search walks the receiver's whole chain (own members first), so
//it is namespace-blind. Without the split, an own static shadows an
//inherited instance method, and a static-only name binds as a
//receiver-bearing handle (silent garbage at runtime).
bool ExprResolveAccessor::ApplyReceiverNamespaceFilter(
	SnIdentifierExpr &idExpr, SnField *&rpField)
{
	auto *pMember = AsMemberInner(idExpr);
	if (!pMember || rpField->Kind() != NK_Function)
		return true;
	auto *pOuter = pMember->Outer();
	if (OuterIsNamespaceQualifier(pOuter))
		return true;
	if (ReceiverNamesType(pOuter))
	{
		//0.9.14 D-new-1: the type-name receiver sees the static namespace,
		//symmetric with the value receiver below — a static method binds as
		//a receiverless function value. An own instance method gives way to
		//the inherited static (AllowAccess gates the rebind;
		//FindMethodInNamespace does not).
		if (rpField->ContainFlags(NF_Static))
			return true;
		auto *pStatic = FindMethodInNamespace(m_pContext, idExpr.Name(), true);
		if (pStatic && pStatic->AllowAccess(*m_pAccessor))
		{
			rpField = pStatic;
			return true;
		}
		m_Env.Log(CLL_Error, idExpr.Location(),
			"the instance method \"%s\" must be called through a receiver "
			"object.",
			idExpr.Name().c_str());
		return false;
	}
	if (!rpField->ContainFlags(NF_Static))
		return true;
	//Value receiver: the instance namespace is the only one it sees — a
	//shadowing own static gives way to the inherited instance method
	//(AllowAccess gates the rebind; FindMethodInNamespace does not).
	auto *pInstance = FindMethodInNamespace(m_pContext, idExpr.Name(), false);
	if (pInstance && pInstance->AllowAccess(*m_pAccessor))
	{
		rpField = pInstance;
		return true;
	}
	m_Env.Log(CLL_Error, idExpr.Location(),
		"the static method \"%s\" must be called through the type name.",
		idExpr.Name().c_str());
	return false;
}

//0.9.10 D2.4: classify the call site by receiver syntax. A bare call
//(any position other than the inner invoke of a member access) keeps the
//namespace-level lookup; a namespace qualifier is not a receiver object
//either (TryResolveModuleQualified owns that path).
InvokeReceiverForm ClassifyInvokeReceiver(const SnInvokeExpr &invoke)
{
	auto *pParent = invoke.Parent();
	if (!(pParent && pParent->Kind() == NK_MemberExpr))
		return InvokeReceiverForm::Bare;
	auto *pMember = static_cast<SnMemberExpr*>(pParent);
	if (pMember->Inner() != &invoke)
		return InvokeReceiverForm::Bare;
	auto *pOuter = pMember->Outer();
	if (pOuter && (pOuter->Kind() == NK_IdentifierExpr
		|| pOuter->Kind() == NK_MemberExpr))
	{
		auto *pField = static_cast<SnFieldExpr*>(pOuter)->Field();
		if (pField && pField->Kind() == NK_Namespace)
			return InvokeReceiverForm::Bare;
	}
	return ReceiverNamesType(pOuter)
		? InvokeReceiverForm::TypeName : InvokeReceiverForm::Value;
}

} //namespace nlang
