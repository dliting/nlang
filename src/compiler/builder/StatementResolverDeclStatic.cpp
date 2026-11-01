/*---
    builder/StatementResolverDeclStatic.cpp — v0.12.6 静态局部变量的
    声明期契约（放置限制与初始化器要求）。从 StatementResolverDecls.cpp
    抽取（源尺寸守卫拆分，零行为变化）。
---*/
#include "StatementResolver.h"

namespace nlang
{

//v0.12.6 D8: static locals are legal only inside a real function body.
//A top-level static local is a module global in disguise (the no-main
//script shape: the statement rides the synthesized entry, which carries
//NF_ScriptEntry); the with-main shape is rejected earlier by the
//script-mode duplicate gate. A generic class's method body has no
//per-instantiation home for the slot (same D8 ban as static fields).
//Placement is the only static-local contract in this task: a legal
//static local still resolves as a plain local here; slot-backed
//storage arrives with the static-local tasks.
void StatementResolveAccessor::RejectStaticLocalPlacement(SnLocalDeclStmt &sn)
{
	auto *pAncestor = sn.Parent();
	while (pAncestor && pAncestor->Kind() != NK_Function)
		pAncestor = pAncestor->Parent();
	if (!pAncestor || pAncestor->ContainFlags(NF_ScriptEntry))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Static local variables must be declared inside a function.");
		return;
	}
	if (m_pCurrClass && m_pCurrClass->IsGenericDeclaration())
		m_Env.Log(CLL_Error, sn.Location(),
			"Static local variables cannot be declared inside a "
			"generic class.");
	else
		RejectStaticConstLocalInit(sn);
}

//v0.12.6 D11: a local static constant is defined by its initializer —
//the initializer-less form is rejected like the field form.
void StatementResolveAccessor::RejectStaticConstLocalInit(
	SnLocalDeclStmt &sn)
{
	if (!(sn.NodeFlags() & NF_Const))
		return;
	for (auto &decl : sn.Decls())
	{
		if (!decl.pInitExpr && !decl.pCtorArgs && !decl.pArraySize)
			m_Env.Log(CLL_Error, sn.Location(),
				"A static constant must have an initializer.");
	}
}

} //namespace nlang
