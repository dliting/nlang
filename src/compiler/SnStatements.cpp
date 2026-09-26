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

SnLocalDeclStmt::SnLocalDeclStmt(SnFieldExpr *pType,
	std::vector<LocalDecl> *pDecls, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pType(pType), m_upDecls(pDecls)
{
	assert(m_pType);
	AddChild(m_pType);
	//Phase 13 Step 0.5 container unification: init expressions are
	//regular children. Ownership lives with the children list until the
	//resolver's decomposition detaches them into SnAssignStmts
	//(SyntaxNode::DetachChild); if resolution never runs, the base
	//destructor still frees them through the list.
	for (auto &decl : *m_upDecls)
		if (decl.pInitExpr)
			AddChild(decl.pInitExpr);
}

//Phase 9a: const local variant.
SnLocalDeclStmt::SnLocalDeclStmt(SnFieldExpr *pType,
	std::vector<LocalDecl> *pDecls, bool isConst,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pType(pType), m_upDecls(pDecls), m_isConst(isConst)
{
	assert(m_pType);
	AddChild(m_pType);
	for (auto &decl : *m_upDecls)
		if (decl.pInitExpr)
			AddChild(decl.pInitExpr);
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

bool SnLocalDeclStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pType == pOld)
	{
		ResetChild(m_pType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	//Init slots share the dual-storage invariant after the container
	//unification; keep every typed slot in sync on splice.
	for (auto &decl : *m_upDecls)
	{
		if (decl.pInitExpr == pOld)
		{
			ResetChild(decl.pInitExpr, static_cast<SnExpression *>(pNew));
			return true;
		}
	}
	return Super_::ReplaceChildNode(pOld, pNew);
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

//SnCompoundAssignStmt

SnCompoundAssignStmt::SnCompoundAssignStmt(SnBinaryExpr::Operator op,
	SnExpression *pLeft, SnExpression *pRight,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_op(op), m_pLeft(pLeft), m_pRight(pRight)
{
	assert(m_pLeft);
	assert(m_pRight);
	AddChild(m_pLeft);
	AddChild(m_pRight);
}

std::string SnCompoundAssignStmt::ToString() const
{
	const char* opStr = "+=";
	switch (m_op) {
	case SnBinaryExpr::OP_Sub: opStr = "-="; break;
	case SnBinaryExpr::OP_Mul: opStr = "*="; break;
	case SnBinaryExpr::OP_Div: opStr = "/="; break;
	case SnBinaryExpr::OP_Mod: opStr = "%="; break;
	default: break;
	}
	return m_pLeft->ToString() + " " + opStr + " " + m_pRight->ToString() + ";\n";
}

void SnCompoundAssignStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnCompoundAssignStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnAssertStmt

SnAssertStmt::SnAssertStmt(SnExpression *pCond, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pCond(pCond)
{
	assert(m_pCond);
	AddChild(m_pCond);
}

std::string SnAssertStmt::ToString() const
{
	return "assert(" + m_pCond->ToString() + ");\n";
}

void SnAssertStmt::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField *SnAssertStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnSubscriptAssignStmt

SnSubscriptAssignStmt::SnSubscriptAssignStmt(SnExpression *pArray,
	SnExpression *pIndex, SnExpression *pValue, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pArray(pArray), m_pIndex(pIndex), m_pValue(pValue)
{
	assert(m_pArray);
	assert(m_pIndex);
	assert(m_pValue);
	AddChild(m_pArray);
	AddChild(m_pIndex);
	AddChild(m_pValue);
}

std::string SnSubscriptAssignStmt::ToString() const
{
	return m_pArray->ToString() + "[" + m_pIndex->ToString() + "] = " +
		m_pValue->ToString() + ";\n";
}

void SnSubscriptAssignStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnSubscriptAssignStmt::FindField(const std::string& sName) const
{
	return nullptr;
}


//SnSuperCallStmt (Phase 9d-2)

SnSuperCallStmt::SnSuperCallStmt(const PtrList<SnExpression>& args,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_args(args)
{
	for (auto* pArg : m_args)
		AddChild(pArg);
}

std::string SnSuperCallStmt::ToString() const
{
	std::stringstream ss;
	ss << "super(";
	bool first = true;
	for (auto* pArg : m_args) {
		if (!first) ss << ", ";
		first = false;
		ss << pArg->ToString();
	}
	ss << ");";
	return ss.str();
}

SnField *SnSuperCallStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

void SnSuperCallStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

}