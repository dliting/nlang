/*-----------------------------------------------------------------------------
	SnStatementsFlow.cpp
	Control-flow and exception statement syntax nodes: if/while/do/for/
	foreach/break/continue, switch case clauses, try/catch/throw. Split
	from SnStatements.cpp (2026-09-27 maintainability refactor, zero
	behavior change).
-----------------------------------------------------------------------------*/

#include "SnStatements.h"
#include <iosfwd>
#include <sstream>
#include <iostream>
#include "SyntaxNodeVisitor.h"

namespace nlang
{

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

//SnDoStmt

SnDoStmt::SnDoStmt(SnExpression *pCond, SnStatement *pBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pCond(pCond), m_pBody(pBody)
{
	assert(m_pCond);
	assert(m_pBody);
	AddChild(m_pCond);
	AddChild(m_pBody);
}

std::string SnDoStmt::ToString() const
{
	return "do " + m_pBody->ToString() + "while (" +
		m_pCond->ToString() + ");\n";
}

void SnDoStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnDoStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnForStmt

SnForStmt::SnForStmt(SnStatement *pInit, SnExpression *pCond,
	SnStatement *pFini, SnStatement *pBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pInit(pInit), m_pCond(pCond),
	m_pFini(pFini), m_pBody(pBody)
{
	assert(m_pCond);
	assert(m_pBody);
	if (m_pInit)
		AddChild(m_pInit);
	AddChild(m_pCond);
	if (m_pFini)
		AddChild(m_pFini);
	AddChild(m_pBody);
}

SnForStmt::~SnForStmt()
{
	for (auto* pExtra : m_initExtras)
		delete pExtra;
	m_initExtras.clear();
}

std::string SnForStmt::ToString() const
{
	std::stringstream ss;
	ss << "for (";
	if (m_pInit)
		ss << m_pInit->ToString();
	ss << "; " << m_pCond->ToString() << "; ";
	if (m_pFini)
		ss << m_pFini->ToString();
	ss << ") " << m_pBody->ToString();
	return ss.str();
}

void SnForStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnForStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnForeachStmt

SnForeachStmt::SnForeachStmt(SnFieldExpr *pVarType, const std::string& varName,
	SnExpression *pIterable, SnStatement *pBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pVarType(pVarType), m_varName(varName),
	m_pIterable(pIterable), m_pBody(pBody)
{
	assert(m_pVarType);
	assert(m_pIterable);
	assert(m_pBody);
	AddChild(m_pVarType);
	AddChild(m_pIterable);
	AddChild(m_pBody);
}

SnForeachStmt::~SnForeachStmt()
{
}

std::string SnForeachStmt::ToString() const
{
	std::stringstream ss;
	ss << "foreach (" << m_pVarType->ToString() << " " << m_varName
	   << " in " << m_pIterable->ToString() << ") "
	   << m_pBody->ToString();
	return ss.str();
}

void SnForeachStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnForeachStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

bool SnForeachStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//The loop variable type is cached in a typed slot beside the children
	//list; keep it in sync when an alias expansion splices the child.
	if (m_pVarType == pOld)
	{
		ResetChild(m_pVarType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//SnBreakStmt

SnBreakStmt::SnBreakStmt(const ISourceLocation &loc) :
	Super_(s_Kind, loc)
{
}

std::string SnBreakStmt::ToString() const
{
	return "break;\n";
}

void SnBreakStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnBreakStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnContinueStmt

SnContinueStmt::SnContinueStmt(const ISourceLocation &loc) :
	Super_(s_Kind, loc)
{
}

std::string SnContinueStmt::ToString() const
{
	return "continue;\n";
}

void SnContinueStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnContinueStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

//SnCaseClause

SnCaseClause::SnCaseClause(PtrList<SnExpression> *pLabels,
	PtrList<SnStatement> *pStmts, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, loc),
	m_upLabels(new std::vector<SnExpression*>(pLabels->begin(), pLabels->end())),
	m_pBody(new SnParagraph(UniquePtrList<SnStatement>(pStmts), loc)),
	m_upChildren(new ImmutableNodeList())
{
	assert(!m_upLabels->empty());  //grammar guarantees ≥1 label
	delete pLabels;
	for (auto* pLabel : *m_upLabels)
		AddChild(pLabel);
	AddChild(m_pBody);
}

std::string SnCaseClause::ToString() const
{
	std::string s = "case ";
	for (size_t i = 0; i < m_upLabels->size(); ++i) {
		if (i) s += ", ";
		s += (*m_upLabels)[i]->ToString();
	}
	return s + ": " + m_pBody->ToString();
}

void SnCaseClause::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnCaseClause::FindField(const std::string& sName) const
{
	return nullptr;
}

ImmutableNodeList *SnCaseClause::ChildrenPtr() const
{
	return m_upChildren.get();
}

//SnSwitchStmt

SnSwitchStmt::SnSwitchStmt(SnExpression *pCond,
	std::vector<SnCaseClause*> *pCases,
	SnParagraph *pDefault, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pCond(pCond),
	m_upCases(pCases), m_pDefault(pDefault)
{
	assert(m_pCond);
	AddChild(m_pCond);
	for (auto* pCase : *m_upCases)
		AddChild(pCase);
	if (m_pDefault)
		AddChild(m_pDefault);
}

SnSwitchStmt::~SnSwitchStmt()
{
	//Case clauses are added as children via AddChild, so they are
	//owned by the child list and will be deleted by Node's destructor.
	//m_upCases is just a reference vector for direct access.
}

std::string SnSwitchStmt::ToString() const
{
	std::stringstream ss;
	ss << "switch (" << m_pCond->ToString() << ") {\n";
	for (auto* pCase : *m_upCases)
		ss << pCase->ToString();
	if (m_pDefault)
		ss << "default: " << m_pDefault->ToString();
	ss << "}\n";
	return ss.str();
}

SnField *SnSwitchStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

void SnSwitchStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

//SnCatchClause (Phase 9d)

SnCatchClause::SnCatchClause(SnFieldExpr *pType, const std::string& varName,
	SnStatement *pBody, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, loc), m_pType(pType),
	m_sVarName(varName), m_pBody(pBody), m_upChildren(new ImmutableNodeList())
{
	assert(m_pType);
	AddChild(m_pType);
	if (m_pBody)
		AddChild(m_pBody);
}

std::string SnCatchClause::ToString() const
{
	return "catch (" + m_pType->ToString() + " " + m_sVarName + ") " +
		(m_pBody ? m_pBody->ToString() : std::string("{}"));
}

bool SnCatchClause::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pType == pOld)
	{
		ResetChild(m_pType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

void SnCatchClause::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

SnField *SnCatchClause::FindField(const std::string& sName) const
{
	return nullptr;
}

ImmutableNodeList *SnCatchClause::ChildrenPtr() const
{
	return m_upChildren.get();
}

//SnTryStmt (Phase 9d)

SnTryStmt::SnTryStmt(SnStatement *pTryBody,
	std::vector<SnCatchClause*> *pCatches, SnStatement *pFinallyBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pTryBody(pTryBody), m_upCatches(pCatches),
	m_pFinallyBody(pFinallyBody)
{
	if (m_pTryBody)
		AddChild(m_pTryBody);
	if (m_upCatches) {
		for (auto* pCatch : *m_upCatches)
			AddChild(pCatch);
	}
	if (m_pFinallyBody)
		AddChild(m_pFinallyBody);
}

SnTryStmt::~SnTryStmt()
{
	//Catches are owned by the child list — no manual delete.
}

std::string SnTryStmt::ToString() const
{
	std::stringstream ss;
	ss << "try " << (m_pTryBody ? m_pTryBody->ToString() : std::string("{}"));
	if (m_upCatches) {
		for (auto* pCatch : *m_upCatches)
			ss << " " << pCatch->ToString();
	}
	if (m_pFinallyBody)
		ss << " finally " << m_pFinallyBody->ToString();
	ss << "\n";
	return ss.str();
}

SnField *SnTryStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

void SnTryStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

//SnThrowStmt (Phase 9d)

SnThrowStmt::SnThrowStmt(SnExpression *pExpr, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pExpr(pExpr)
{
	if (m_pExpr)
		AddChild(m_pExpr);
}

std::string SnThrowStmt::ToString() const
{
	if (m_pExpr)
		return "throw " + m_pExpr->ToString() + ";";
	return "throw;";
}

SnField *SnThrowStmt::FindField(const std::string& sName) const
{
	return nullptr;
}

void SnThrowStmt::Accept(nlang::ISyntaxNodeVisitor& v)
{
	v.Visit(*this);
}

} //namespace nlang
