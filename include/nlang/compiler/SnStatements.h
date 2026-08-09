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
		m_pTypeField(pTypeField), m_bIsArray(false)
	{
	}

	SnField *EvalDataType() const override
	{
		return m_pTypeField;
	}

	bool IsArrayType() const override
	{
		return m_bIsArray;
	}

	void SetArrayType(bool b)
	{
		m_bIsArray = b;
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
	bool m_bIsArray;
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

	SnLocalDeclStmt(SnFieldExpr *pType,
		std::vector<LocalDecl> *pDecls, const ISourceLocation &loc);

	//Phase 9a: const local variant. isConst=true marks all declared
	//locals as NF_Const (assignment after declaration = compile error,
	//declaration must include an initializer).
	SnLocalDeclStmt(SnFieldExpr *pType,
		std::vector<LocalDecl> *pDecls, bool isConst,
		const ISourceLocation &loc);

	~SnLocalDeclStmt() override;

	//Get the type name expression.
	SnFieldExpr *Type() const { return m_pType; }

	//Phase 9a: const-ness flag.
	bool IsConst() const { return m_isConst; }

	//Get the declarations.
	const std::vector<LocalDecl> &Decls() const { return *m_upDecls; }
	std::vector<LocalDecl> &Decls() { return *m_upDecls; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnFieldExpr *m_pType;
	std::unique_ptr<std::vector<LocalDecl>> m_upDecls;
	bool m_isConst = false;
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

//Compound assignment statement (e.g. x += 1, arr[i] *= 2).
//Phase 9a: left-value is evaluated only once (read-modify-write).
class NLANG_COMPILER_API SnCompoundAssignStmt : public SnStatement
{
	friend class StatementResolveAccessor;
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_CompoundAssignStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	//op: the underlying binary operator (OP_Add, OP_Sub, OP_Mul, OP_Div, OP_Mod)
	SnCompoundAssignStmt(SnBinaryExpr::Operator op,
		SnExpression *pLeft, SnExpression *pRight,
		const ISourceLocation &loc);

	SnBinaryExpr::Operator Op() const { return m_op; }
	SnExpression *Left() const { return m_pLeft; }
	SnExpression *Right() const { return m_pRight; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnBinaryExpr::Operator m_op;
	SnExpression *m_pLeft;
	SnExpression *m_pRight;
};

//Assert statement: assert(cond); - exit(1) on failure.
//Phase 9a: simple runtime check, prints source location + "assertion failed".
class NLANG_COMPILER_API SnAssertStmt : public SnStatement
{
	friend class StatementResolveAccessor;
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_AssertStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnAssertStmt(SnExpression *pCond, const ISourceLocation &loc);

	SnExpression *Cond() const { return m_pCond; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pCond;
};

//Array subscript assignment statement (e.g. arr[i] = value).
class NLANG_COMPILER_API SnSubscriptAssignStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_SubscriptAssignStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnSubscriptAssignStmt(SnExpression *pArray, SnExpression *pIndex,
		SnExpression *pValue, const ISourceLocation &loc);

	SnExpression *Array() const { return m_pArray; }
	SnExpression *Index() const { return m_pIndex; }
	SnExpression *Value() const { return m_pValue; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pArray;
	SnExpression *m_pIndex;
	SnExpression *m_pValue;
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
Do-while loop statement.
Reference: EN's DoStmt (SeStatements.h:449).
*/
class NLANG_COMPILER_API SnDoStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_DoStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnDoStmt(SnExpression *pCond, SnStatement *pBody,
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
Foreach loop statement.
Reference: NLang Phase 8e-5 — iterates Array / List<T> / Dict<K,V> (keys).
*/
class NLANG_COMPILER_API SnForeachStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_ForeachStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnForeachStmt(SnFieldExpr *pVarType, const std::string& varName,
		SnExpression *pIterable, SnStatement *pBody,
		const ISourceLocation &loc);

	~SnForeachStmt() override;

	SnFieldExpr *VarType() const { return m_pVarType; }
	const std::string& VarName() const { return m_varName; }
	SnExpression *Iterable() const { return m_pIterable; }
	SnStatement *Body() const { return m_pBody; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnFieldExpr  *m_pVarType;
	std::string   m_varName;
	SnExpression *m_pIterable;
	SnStatement  *m_pBody;
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

/*
Case clause in a switch statement.
Reference: EN's CondClause (SeStatements.h:141).
*/
class NLANG_COMPILER_API SnCaseClause : public SyntaxNode
{
	typedef SyntaxNode Super_;
public:
	static const NodeKind	s_Kind			= NK_CaseClause;
	static const NodeBits	s_DefaultFlags	= NF_NONE;

	SnCaseClause(SnExpression *pCond, PtrList<SnStatement> *pStmts,
		const ISourceLocation &loc);

	SnExpression *Cond() const { return m_pCond; }
	SnParagraph *Body() const { return m_pBody; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	ImmutableNodeList *ChildrenPtr() const override;
	SnExpression *m_pCond;
	SnParagraph *m_pBody;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

/*
Switch statement.
Reference: EN's SwitchStmt (SeStatements.h:522).
*/
class NLANG_COMPILER_API SnSwitchStmt : public SnStatement
{
	typedef SnStatement Super_;
public:
	static const NodeKind	s_Kind			= NK_SwitchStmt;
	static const NodeBits	s_DefaultFlags	= NF_Statement;

	SnSwitchStmt(SnExpression *pCond,
		std::vector<SnCaseClause*> *pCases,
		SnParagraph *pDefault,
		const ISourceLocation &loc);

	~SnSwitchStmt() override;

	SnExpression *Cond() const { return m_pCond; }
	std::vector<SnCaseClause*> &Cases() { return *m_upCases; }
	const std::vector<SnCaseClause*> &Cases() const { return *m_upCases; }
	SnParagraph *Default() const { return m_pDefault; }

	std::string ToString() const override;
	SnField *FindField(const std::string& sName) const override;
	void Accept(nlang::ISyntaxNodeVisitor&) override;
private:
	SnExpression *m_pCond;
	std::unique_ptr<std::vector<SnCaseClause*>> m_upCases;
	SnParagraph *m_pDefault;
};

} //namespace nlang