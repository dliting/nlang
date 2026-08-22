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

SnLocalDeclStmt::SnLocalDeclStmt(SnFieldExpr *pType,
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

//Phase 9a: const local variant.
SnLocalDeclStmt::SnLocalDeclStmt(SnFieldExpr *pType,
	std::vector<LocalDecl> *pDecls, bool isConst,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pType(pType), m_upDecls(pDecls), m_isConst(isConst)
{
	assert(m_pType);
	AddChild(m_pType);
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