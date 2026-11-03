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

//v0.12.6 Task 7 + D14: the type contract of a static local. The sized
//array declarator is rejected: it allocates a fresh array per
//execution, and the per-declarator array-token typing stays out of the
//static-local path — an array-typed declaration initialized with new
//covers the use. Struct-typed static locals are legal since D14: the
//slot's store deep-copies at the runtime choke point (OpStoreStatic),
//and an initializer-less declaration materializes a zeroed record at
//run init — the same value semantics as any other struct storage.
void StatementResolveAccessor::RejectStaticLocalTypeContract(
	SnLocalDeclStmt &sn)
{
	for (auto &decl : sn.Decls())
	{
		if (decl.pArraySize)
			m_Env.Log(CLL_Error, sn.Location(),
				"Static local \"%s\" cannot use a sized array "
				"declarator; declare an array type and initialize it "
				"with new.",
				decl.name.c_str());
	}
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
