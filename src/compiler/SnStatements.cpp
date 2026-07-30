/*-----------------------------------------------------------------------------
	ncomp/intf/SnStatements.cpp
	This file define the implementation of statement syntax nodes in an nlang 
AST.
-----------------------------------------------------------------------------*/

#include "SnStatements.h"
#include <iosfwd>
#include <sstream>
#include <iostream>
#include "SyntaxNodeVisitor.h"

namespace nlang
{

SnStatement::SnStatement(NodeKind k, const ISourceLocation& loc) :
	Super_(k, FA_Public, NF_Statement, loc),
	m_upChildren(new ImmutableNodeList())
{
}

SnStatement::~SnStatement()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

ImmutableNodeList * SnStatement::ChildrenPtr() const
{
	return m_upChildren.get();
}

SnParagraph::SnParagraph(UniquePtrList<SnStatement> upStmts,
	const ISourceLocation& loc) :
	Super_(s_Kind, loc),
	m_upStatements(CreateChildNodes(upStmts, this))
{
}

SnParagraph::~SnParagraph()
{
	//SnLocalVar descriptors are allocated via new in StatementResolver and
	//stored as raw pointers in m_Locals. They are not AST children (not
	//owned by the child list), so we must release them here.
	for (auto &pair : m_Locals)
	{
		auto *pLocal = pair.second;
		if (pLocal && pLocal->Kind() == NK_FormalParam)
			delete pLocal;
	}
}

std::string SnParagraph::ToString() const
{
	return "!Paragraph";
}

void SnParagraph::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField * SnParagraph::FindField(const std::string& sName) const
{
	//Search in local variables first, then delegate to parent.
	//Reference: EN's CompositeStatement::FindField (SeStatements.cpp:57).
	SnField *pLocal = FindLocal(sName);
	if (pLocal)
		return pLocal;
	//Delegate to parent's FindField
	auto pParent = Parent();
	if (pParent)
		return pParent->FindField(sName);
	return nullptr;
}

bool SnParagraph::AddLocal(const std::string &sName, SnField *pField)
{
	assert(pField);
	auto it = m_Locals.find(sName);
	if (it != m_Locals.end())
		return false;  //duplicate
	m_Locals[sName] = pField;
	//Do NOT call pField->Parent(this) — SnLocalVar is a lightweight
	//descriptor that must not be added as a child of this Paragraph.
	//Unlike regular AST nodes, SnLocalVar only exists in the m_Locals
	//lookup table and does not participate in the child list.
	return true;
}

SnField *SnParagraph::FindLocal(const std::string &sName) const
{
	auto it = m_Locals.find(sName);
	if (it != m_Locals.end())
		return it->second;
	return nullptr;
}

SnReturnStmt::SnReturnStmt(SnExpression* pResult, const ISourceLocation& loc) :
	Super_(s_Kind, loc), m_pResult(pResult)
{
	if (m_pResult)
		AddChild(m_pResult);
}

std::string SnReturnStmt::ToString() const
{
	if (!m_pResult)
		return "return;\n";
	return "return " + m_pResult->ToString() + ";\n";
}

void SnReturnStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField * SnReturnStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

SnInvokeStmt::SnInvokeStmt(SnMemberExpr* pExpr, const ISourceLocation& loc) :
	Super_(s_Kind, loc), m_pExpr(pExpr)
{
	assert(m_pExpr);
	m_pExpr->Checker(&SnMemberExpr::InvokeExprChecker());
	AddChild(m_pExpr);
}

SnInvokeStmt::SnInvokeStmt(SnInvokeExpr *pExpr, const ISourceLocation& loc) :
	Super_(s_Kind, loc), m_pExpr(pExpr)
{
	assert(m_pExpr);
	AddChild(m_pExpr);
}

std::string SnInvokeStmt::ToString() const
{
	if (m_pExpr)
		return m_pExpr->ToString() + ";\n";
	return "";
}

void SnInvokeStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField * SnInvokeStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnLocalDeclStmt

SnLocalDeclStmt::SnLocalDeclStmt(SnNameExpr *pType,
	std::vector<LocalDecl> *pDecls, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pType(pType), m_upDecls(pDecls)
{
	assert(m_pType);
	AddChild(m_pType);
	//Init expressions are NOT added as children here. They are temporary
	//ownership — the decomposition pattern in StatementResolver transfers
	//them to SnAssignStmt nodes. If resolution never happens (e.g. type
	//unresolved), the destructor cleans them up.
	//Reference: EN's DataDeclBase stores m_pDefaultValue as a raw pointer
	//without calling AddChild.
}

SnLocalDeclStmt::~SnLocalDeclStmt()
{
	//Clean up any init expressions that were not transferred to AssignStmts
	//(i.e. resolution never ran, typically because the type was unresolved).
	for (auto& decl : *m_upDecls)
	{
		if (decl.pInitExpr)
		{
			delete decl.pInitExpr;
			decl.pInitExpr = nullptr;
		}
	}
}

std::string SnLocalDeclStmt::ToString() const
{
	std::stringstream ss;
	ss << m_pType->ToString() << " ";
	for (size_t i = 0; i < Decls().size(); ++i)
	{
		if (i > 0)
			ss << ", ";
		ss << Decls()[i].name;
		if (Decls()[i].pInitExpr)
			ss << " = " << Decls()[i].pInitExpr->ToString();
	}
	ss << ";\n";
	return ss.str();
}

void SnLocalDeclStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnLocalDeclStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnAssignStmt

SnAssignStmt::SnAssignStmt(SnExpression *pLeft, SnExpression *pRight,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pLeft(pLeft), m_pRight(pRight)
{
	assert(m_pLeft);
	assert(m_pRight);
	AddChild(m_pLeft);
	AddChild(m_pRight);
}

std::string SnAssignStmt::ToString() const
{
	return m_pLeft->ToString() + " = " + m_pRight->ToString() + ";\n";
}

void SnAssignStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnAssignStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnIfStmt

SnIfStmt::SnIfStmt(SnExpression *pCond, SnStatement *pThen,
	SnStatement *pElse, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pCond(pCond), m_pThen(pThen), m_pElse(pElse)
{
	assert(m_pCond);
	assert(m_pThen);
	AddChild(m_pCond);
	AddChild(m_pThen);
	if (m_pElse)
		AddChild(m_pElse);
}

std::string SnIfStmt::ToString() const
{
	std::stringstream ss;
	ss << "if (" << m_pCond->ToString() << ") " << m_pThen->ToString();
	if (m_pElse)
		ss << "else " << m_pElse->ToString();
	return ss.str();
}

void SnIfStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnIfStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnWhileStmt

SnWhileStmt::SnWhileStmt(SnExpression *pCond, SnStatement *pBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pCond(pCond), m_pBody(pBody)
{
	assert(m_pCond);
	assert(m_pBody);
	AddChild(m_pCond);
	AddChild(m_pBody);
}

std::string SnWhileStmt::ToString() const
{
	return "while (" + m_pCond->ToString() + ") " + m_pBody->ToString();
}

void SnWhileStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnWhileStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

}