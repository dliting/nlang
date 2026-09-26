/*---
    StatementResolver.cpp — 语句解析入口与轻量访问器：构造、表达式/段落等小型 Access 重载、StatementResolver 驱动。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"

namespace nlang
{

StatementResolveAccessor::StatementResolveAccessor(BuildEnvironment &env) :
	m_Env(env), m_pCurrType(nullptr), m_pVisitor(nullptr),
	m_ExprResolver(m_Env)
{
}

void StatementResolveAccessor::Visitor(ISyntaxNodeVisitor *pVisitor)
{
	m_pVisitor = pVisitor;
}

void StatementResolveAccessor::Access(SnNamespace &sn)
{
	assert(m_pVisitor);
	for (auto& field : sn.Members())
	{
		if (CanBeFuncParentEx(field.Kind()) || field.Kind() == NK_Function || field.Kind() == NK_EnumDecl || field.Kind() == NK_StructDecl || field.Kind() == NK_ClassDecl)
			field.Accept(*m_pVisitor);
	}
}

void StatementResolveAccessor::Access(SnField &sn)
{
	m_pCurrType = nullptr;
}

void StatementResolveAccessor::Access(SnExpression &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pCurrType);
	assert(sn.Parent());
	m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
}

void StatementResolveAccessor::Access(SnParagraph &sn)
{
	assert(m_pVisitor);
	for (auto &stmt : sn.Statements())
	{
		stmt.Accept(*m_pVisitor);
	}
}

void StatementResolveAccessor::Access(SnNewExpr &sn)
{
	m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
}

void StatementResolveAccessor::Access(SnInvokeStmt &sn)
{
	assert(m_pVisitor);
	sn.Expr()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnAssertStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckIntCondition(*sn.Cond(), "assert");
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnThisExpr &sn)
{
	m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
}

void StatementResolveAccessor::Access(SnArrayTypeExpr &)
{
	//Type expressions are resolved via ExprResolver when used in
	//declarations; nothing to do at statement level.
}

void StatementResolveAccessor::Access(SyntaxNode &sn)
{
}

StatementResolver::StatementResolver(BuildEnvironment &env) :	m_Accessor(env)
{
}

//Pre-pass for Resolve: run Access(SnEnumDecl) on every enum decl in
//the tree before normal document-order traversal. The ResolveDataTypes
//pass flags enum decls NF_Resolved WITHOUT assigning member values, so
//a forward-referenced enum (a function above the decl using
//`case Color.Red`) would otherwise read stale 0s — e.g. false
//"duplicate case label" errors. Value assignment is pure literal work,
//so pre-pass + in-order Access is idempotent.
void StatementResolver::PreAssignEnumMemberValues(Node& node,
	StatementResolveAccessor& accessor)
{
	if (node.Kind() == NK_EnumDecl)
		accessor.AssignEnumMemberValues(static_cast<SnEnumDecl&>(node));
	for (auto& child : node.Children())
		PreAssignEnumMemberValues(child, accessor);
}

void StatementResolver::Resolve(SnNamespace &root)
{
	SyntaxNodeVisitor<StatementResolveAccessor>
		visitor(m_Accessor, NVK_CustomTraverse);
	m_Accessor.Visitor(&visitor);
	PreAssignEnumMemberValues(root, m_Accessor);
	root.Accept(visitor);
}

} //namespace nlang
