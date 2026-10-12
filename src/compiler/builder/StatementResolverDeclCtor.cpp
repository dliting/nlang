/*---
    StatementResolverDeclCtor.cpp — 直接初始化声明的合成降级：
    `T name(args);` 分解为与 `T name = new T(args);` 相同的赋值语句树。
    自 StatementResolverDecls.cpp 抽出（0.9.0，源文件 500 行守卫）。
---*/
#include "StatementResolver.h"
#include <list>

namespace nlang
{

SnAssignStmt *StatementResolveAccessor::BuildCtorInitAssign(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl)
{
	//Generic type heads (SnGenericTypeExpr) have no class declaration a
	//fresh SnNewExpr could bind — reject by name instead of letting
	//Access(SnNewExpr&) produce its generic not-a-class text.
	if (sn.Type()->Kind() == NK_GenericTypeExpr)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"generic class \"%s\" does not support direct-init; use [] or "
			"{} with a typed declaration",
			sn.Type()->ToString().c_str());
		return nullptr;
	}
	//The fresh class-name field binds the ALREADY-RESOLVED declaration
	//type directly (ResolveFieldExprAs): value-position name lookup would
	//find shadowing locals first, and built-in classes have no namespace
	//entry to find — the same bypass TryResolveBuiltinClassName uses.
	auto *pClassName = new SnIdentifierExpr(
		new std::string(sn.Type()->ToString()), *sn.Location());
	m_ExprResolver.ResolveFieldExprAs(*pClassName, sn.Type()->Field());
	//Detach the args from this statement's children list (dual storage)
	//into the SnNewExpr; the caller drops the emptied payload container.
	//The args list takes the same inner-collection form the grammar's
	//ConcreteParamList hands to SnNewExpr (an owning UniquePtrList is
	//built from it inside the call).
	auto *pArgs = new std::list<SnExpression *>();
	for (auto *pArg : *decl.pCtorArgs)
		pArgs->push_back(sn.DetachChild(pArg));
	auto *pNew = new SnNewExpr(pClassName, pArgs, *sn.Location());
	auto *pLeft = new SnIdentifierExpr(
		new std::string(decl.name), *sn.Location());
	return new SnAssignStmt(pLeft, pNew, *sn.Location());
}

//Consume a `Type name(args)` declarator: build the decomposition assign,
//insert it at iInsert and resolve it. Accept(SnAssignStmt&) drives
//Access(SnNewExpr&) (class check, arity, argument resolution) and the
//LHS/cast tail. The emptied ctor-arg container is deleted either way so
//the consumed slots never reach codegen.
void StatementResolveAccessor::SynthesizeCtorInitDeclarator(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl,
	SnParagraph &paragraph, NodeIterator iInsert)
{
	auto *pAssign = BuildCtorInitAssign(sn, decl);
	delete decl.pCtorArgs;
	decl.pCtorArgs = nullptr;
	if (pAssign)
	{
		paragraph.InsertChild(iInsert, pAssign);
		pAssign->Accept(*m_pVisitor);
	}
}

} //namespace nlang
