/*-----------------------------------------------------------------------------
	ncomp/intf/SnStatements.h
	This file define the interface of statement syntax nodes in an nlang AST.
-----------------------------------------------------------------------------*/

#pragma once
#include "SnExpressions.h"
#include <memory>
#include <vector>
#include <map>

namespace nlang
{

/*
Lightweight local variable descriptor.
Unlike SnFormalParam (which owns its type and init expressions via AddChild),
SnLocalVar only stores a reference to the resolved type field. This avoids
double-parenting: the type expression is already owned by SnLocalDeclStmt,
so we cannot pass it to another node that calls AddChild.
Reference: EN's LocalDecl (SeBasics.h:823), which stores m_pDataType as a
raw pointer without taking ownership.
*/
class NLANG_COMPILER_API SnLocalVar : public SnField
{
	typedef SnField Super_;
public:
	//SnLocalVar is not a real AST node — it's a lightweight descriptor
	//used for local variable registration. It uses NK_FormalParam as
	//its kind because it's semantically similar (a named variable slot)
	//but doesn't participate in the Visitor pattern.

	SnLocalVar(const std::string &sName, SnField *pTypeField,
		const ISourceLocation &loc) :
		Super_(NK_FormalParam, FA_Public, NF_NONE,
			new std::string(sName), loc),
		m_pTypeField(pTypeField)
	{
	}

	SnField *EvalDataType() const override
	{
		return m_pTypeField;
	}

	SnField *FindField(const std::string&) const override
	{
		return nullptr;
	}

	void Accept(ISyntaxNodeVisitor&) override
	{
		//SnLocalVar is a lightweight descriptor, not a real AST node.
	}
	ImmutableNodeList *ChildrenPtr() const override
	{
		return nullptr;
	}

private:
	SnField *m_pTypeField;
};

//The abstract base class of a statement syntax node.
class NLANG_COMPILER_API SnStatement: public SyntaxNode
{
	typedef SyntaxNode Super_;
public:
	SnStatement(NodeKind, const ISourceLocation&);

	~SnStatement() override;
private:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

typedef MutableChildNodeList<SnStatement> SnStatementList;

/*
A group of sequential statements.
A paragraph would include some local declarations(variables/constants).
*/
class NLANG_COMPILER_API SnParagraph: public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_Paragraph;
	static const NodeBits	s_DefaultFlags	= NF_Statement;
public:
	SnParagraph(UniquePtrList<SnStatement> upStmts, const ISourceLocation &);

	//Get the statements in this paragraph.
	//@{
	const SnStatementList& Statements() const
	{
		return *m_upStatements;
	}

	SnStatementList& Statements()
	{
		return *m_upStatements;
	}
	//@}

	//Register a local variable.
	//Reference: EN's CompositeStatement::AddLocal (SeStatements.cpp:37).
	bool AddLocal(const std::string &sName, SnField *pField);

	//Find a local variable by name.
	//Reference: EN's CompositeStatement::FindField (SeStatements.cpp:57).
	SnField *FindLocal(const std::string &sName) const;

	std::string ToString() const override;

	SnField *FindField(const std::string& sName) const override;

	void Accept(nlang::ISyntaxNodeVisitor&) override;

	~SnParagraph() override;
private:
	std::unique_ptr<SnStatementList> m_upStatements;
	std::map<std::string, SnField*> m_Locals;
};

//The syntax node represent a "return" statement.
class NLANG_COMPILER_API SnReturnStmt: public SnStatement
{
	friend class StatementResolveAccessor;
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_ReturnStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;
public:
	/*
	Constructor.
	The result expression will be owned by this statement.
	*/
	explicit SnReturnStmt(SnExpression* pResult, const ISourceLocation&);

	//Get the result.
	SnExpression* Result() const
	{
		return m_pResult;
	}

	std::string ToString() const override;

	SnField *FindField(const std::string& sName) const override;

	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pResult;
};

class NLANG_COMPILER_API SnInvokeStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_InvokeStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;
public:
	SnInvokeStmt(SnMemberExpr *, const ISourceLocation&);

	SnInvokeStmt(SnInvokeExpr *, const ISourceLocation&);

	//Get the invoke expression.
	SnExpression *Expr() const
	{
		return m_pExpr;
	}

	std::string ToString() const override;

	SnField *FindField(const std::string& sName) const override;

	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnFieldExpr *m_pExpr;
};

/*
Local variable declaration statement.
Reference: EN's LocalDeclStmt (SeStatements.h:230).
*/
class NLANG_COMPILER_API SnLocalDeclStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_LocalDeclStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	//A single variable declaration.
	struct LocalDecl
	{
		std::string name;
		SnExpression *pInitExpr;  //may be nullptr
	};

	SnLocalDeclStmt(SnNameExpr *pType,
		std::vector<LocalDecl> *pDecls, const ISourceLocation &loc);

	~SnLocalDeclStmt() override;

	//Get the type name expression.
	SnNameExpr *Type() const { return m_pType; }

	//Get the declarations.
	const std::vector<LocalDecl> &Decls() const { return *m_upDecls; }
	std::vector<LocalDecl> &Decls() { return *m_upDecls; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnNameExpr *m_pType;
	std::unique_ptr<std::vector<LocalDecl>> m_upDecls;
};

/*
Assignment statement.
Reference: EN's AssignStmt (SeStatements.h:267).
*/
class NLANG_COMPILER_API SnAssignStmt : public SnStatement
{
	friend class StatementResolveAccessor;
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_AssignStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnAssignStmt(SnExpression *pLeft, SnExpression *pRight,
		const ISourceLocation &loc);

	SnExpression *Left() const { return m_pLeft; }
	SnExpression *Right() const { return m_pRight; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pLeft;
	SnExpression *m_pRight;
};

/*
If/else statement.
Reference: EN's IfStmt (SeStatements.h:324).
*/
class NLANG_COMPILER_API SnIfStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_IfStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnIfStmt(SnExpression *pCond, SnStatement *pThen,
		SnStatement *pElse, const ISourceLocation &loc);

	SnExpression *Cond() const { return m_pCond; }
	SnStatement *ThenStmt() const { return m_pThen; }
	SnStatement *ElseStmt() const { return m_pElse; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pCond;
	SnStatement *m_pThen;
	SnStatement *m_pElse;
};

/*
While loop statement.
Reference: EN's WhileStmt (SeStatements.h:428).
*/
class NLANG_COMPILER_API SnWhileStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_WhileStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnWhileStmt(SnExpression *pCond, SnStatement *pBody,
		const ISourceLocation &loc);

	SnExpression *Cond() const { return m_pCond; }
	SnStatement *Body() const { return m_pBody; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pCond;
	SnStatement *m_pBody;
};

/*
For loop statement.
Reference: EN's ForStmt (SeStatements.h:470).
*/
class NLANG_COMPILER_API SnForStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_ForStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnForStmt(SnStatement *pInit, SnExpression *pCond,
		SnStatement *pFini, SnStatement *pBody,
		const ISourceLocation &loc);

	~SnForStmt() override;

	SnStatement *Init() const { return m_pInit; }
	SnExpression *Cond() const { return m_pCond; }
	SnStatement *Fini() const { return m_pFini; }
	SnStatement *Body() const { return m_pBody; }

	//Decomposition extras: AssignStmts from init LocalDeclStmt decomposition.
	//Reference: EN's ForStmt child list contains decomposed AssignStmts.
	std::vector<SnAssignStmt*> &InitExtras() { return m_initExtras; }
	const std::vector<SnAssignStmt*> &InitExtras() const { return m_initExtras; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnStatement *m_pInit;
	SnExpression *m_pCond;
	SnStatement *m_pFini;
	SnStatement *m_pBody;
	std::vector<SnAssignStmt*> m_initExtras;
};

/*
Break statement.
Reference: EN's BreakStmt (SeStatements.h:596).
*/
class NLANG_COMPILER_API SnBreakStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_BreakStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	explicit SnBreakStmt(const ISourceLocation &loc);

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
};

/*
Continue statement.
Reference: EN's ContinueStmt (SeStatements.h:617).
*/
class NLANG_COMPILER_API SnContinueStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_ContinueStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	explicit SnContinueStmt(const ISourceLocation &loc);

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
};

} //namespace nlang