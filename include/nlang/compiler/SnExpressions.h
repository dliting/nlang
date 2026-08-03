/*-----------------------------------------------------------------------------
	ncomp/intf/SnExpressions.h
	This file define the interfaces of expression syntax nodes in an nlang AST.
-----------------------------------------------------------------------------*/

#pragma once
#include "SyntaxNode.h"
#include "CastInfo.h"
#include <nlang/runtime/Variant.h>
#include <nlang/runtime/NodeContainers.h>
#include <nlang/runtime/AutoPointers.h>
#include <functional>
#include <memory>

#ifdef NLANG_ENABLE_LLVM
namespace llvm
{

class Value;

} //namespace llvm
#endif

namespace nlang
{

//The abstract expression syntax node.
class NLANG_COMPILER_API SnExpression: public SyntaxNode
{
	friend class ExprResolveAccessor;
	friend class StatementGenerateAccessor;
	typedef SyntaxNode Super_;
public:
	SnExpression(NodeKind);

	SnExpression(NodeKind, const ISourceLocation &);

	//Get the resolved data type of this expression.
	SnField *EvalDataType() const
	{
		return m_pEvalDataType;
	}

	//llvm::Type *MetaType() const
	//{
	//	return m_pEvalDataType ? m_pEvalDataType->MetaType() : nullptr;
	//}

#ifdef NLANG_ENABLE_LLVM
	//Get the LLVM value that evaluated.
	llvm::Value *MetaValue() const
	{
		return m_pMetaValue;
	}
#endif

	SnField *FindField(const std::string& sName) const override;

	//Get the owner type the expression is used in.
	SnField *OwnerType() const;

	//Is this a data expression?
	//A data expression is a data value.
	virtual bool IsDataExpr() const = 0;
protected:
	//Set the the resolved data type of this expression.
	void EvalDataType(SnField *pType)
	{
		m_pEvalDataType = pType;
	}

	//Return a null child list by default.
	ImmutableNodeList *ChildrenPtr() const override;
private:
	SnField *m_pEvalDataType;
#ifdef NLANG_ENABLE_LLVM
	llvm::Value *m_pMetaValue;
#endif
};

/*
The base class of a ordinary expression composites by other expressions.
\See SnCompoundFieldExpr.
*/
class NLANG_COMPILER_API SnCompoundPlainExpr : public SnExpression
{
	typedef SnExpression Super_;
public:
	explicit SnCompoundPlainExpr(NodeKind);

	SnCompoundPlainExpr(NodeKind, const ISourceLocation &);

	~SnCompoundPlainExpr() override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

class BuildEnvironment;

//The base class of the expression refers to a field.
class NLANG_COMPILER_API SnFieldExpr : public SnExpression
{
	friend class ExprResolveAccessor;
	friend class SyntaxTree;
	typedef SnExpression Super_;
public:
	typedef std::function<bool (SnFieldExpr&, BuildEnvironment&)> FieldChecker;
public:
	explicit SnFieldExpr(NodeKind);

	SnFieldExpr(NodeKind, const ISourceLocation &);

	//Get the source field referred by this name expression.
	SnField *Field() const
	{
		return m_pField;
	}

	//Set the type checker.
	void Checker(FieldChecker *pChecker)
	{
		m_pChecker = pChecker;
	}

	/*
	Verify the referred field after the it is resolved.
	This method is often used to verify the referred field according to the
	parent type of this expression.
	Precondition: Field() != nullptr.
	*/
	bool PostResolveCheck(BuildEnvironment &env)
	{
		assert(m_pField);
		if (!m_pChecker)  //Needn't to be checked.
			return true;
		return (*m_pChecker)(*this, env);
	}

	virtual bool IsDataExpr() const override;

	//Is this expression an array type (e.g. int[] or T[])?
	//Subclasses representing array types override this to return true.
	//This is the polymorphic hook used by SnField subclasses (via their
	//m_pType pointer) to forward IsArrayType queries.
	virtual bool IsArrayType() const
	{
		return false;
	}

	static SnFieldExpr::FieldChecker& DataTypeChecker();
protected:
	SnField *m_pField;
private:
	FieldChecker *m_pChecker;
};

//The expression composite by other expressions.
class NLANG_COMPILER_API SnCompoundFieldExpr : public SnFieldExpr
{
	typedef SnFieldExpr Super_;
public:
	explicit SnCompoundFieldExpr(NodeKind);

	SnCompoundFieldExpr(NodeKind, const ISourceLocation &);

	~SnCompoundFieldExpr() override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

typedef MutableChildNodeList<SnExpression> SnExpressionList;

//The syntax node of a literal constant expression.
class NLANG_COMPILER_API SnLiteralExpr: public SnExpression
{
	typedef SnExpression Super_;
public:
	static const NodeKind	s_Kind			= NK_LiteralExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression | NF_Const;
public:
	/*
	Construct a literal constant expression by a given C++ value.
	\param type		The runtime constant value type.
	\param value	The C++ value.
	\param loc		The source location of this expression.
	*/
	template<class RTTI_T>
	SnLiteralExpr(RTTI_T &type, typename RTTI_T::CppType value,
		const ISourceLocation &loc) :
		Super_(s_Kind, loc), m_Value(type, value)
	{
		EvalByRTTI(type);
		AddFlags(NF_Imported);
	}

	template<typename CPP_T>
	SnLiteralExpr(NodeKind valueKind, CPP_T value, const ISourceLocation &loc) :
		Super_(s_Kind, loc), m_Value(valueKind, value)
	{
		assert(m_Value->Type());
	}

	SnLiteralExpr(RnDataType &, void *pValue);

	~SnLiteralExpr() override;

	Variant &Value()
	{
		return m_Value;
	}

	virtual bool IsDataExpr() const override;

	void Accept(nlang::ISyntaxNodeVisitor&) override;

	std::string ToString() const override;
private:
	void EvalByRTTI(RnDataType &rtti);
	Variant m_Value;
};

/*
The reference to a single identifier.
It can be one of the following forms:
1) A primitive type such as "int", "string", etc.
2) A single field non-primitive type, such as "Object", "MyClass", etc.
3) A named value such as "i", "value", etc.
*/
class NLANG_COMPILER_API SnIdentifierExpr : public SnFieldExpr
{
	typedef SnFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_IdentifierExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnIdentifierExpr(NodeKind builtinKind, const ISourceLocation &loc);

	SnIdentifierExpr(std::string *pName, const ISourceLocation &loc);

	~SnIdentifierExpr() override;

	std::string Name() const
	{
		assert(m_upName || Field());
		return m_upName ? *m_upName : Field()->Name();
	}

	const std::string &MetaName() const
	{
		return Field() ? Field()->MetaName() : EmptyMetaName();
	}

	void Accept(ISyntaxNodeVisitor &) override;

	std::string ToString() const override;
private:
	std::unique_ptr<std::string> m_upName;
};

class SnFunction;

//The syntax node of a function call.
class NLANG_COMPILER_API SnInvokeExpr : public SnCompoundFieldExpr
{
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_InvokeExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	/*
	Construct a invoke expression by a given value.
	\param pCalleeName	The function name.
	\param pParams		The concrete parameters.
	\param loc			The source location of this expression.
	*/
	SnInvokeExpr(std::string *pCalleeName,
		UniquePtrList<SnExpression> upParams, const ISourceLocation &loc);

	~SnInvokeExpr() override;

	//Get the param expressions.
	SnExpressionList &Params() const
	{
		return *m_pParams;
	}

	//Get the function name been called.
	const std::string &CalleeName() const
	{
		return *m_upCalleeName;
	}

	SnFunction *Callee() const
	{
		assert(!Field() || Field()->Kind() == NK_Function);
		return (SnFunction *)(Field());
	}

	void Accept(nlang::ISyntaxNodeVisitor&) override;

	std::string ToString() const override;
private:
	std::unique_ptr<std::string> m_upCalleeName;
	SnExpressionList* m_pParams;
};

/*
Member field access expression.
For example, "Foo.Bar", "MyNamespace.MyClass", etc.
Every member access express has two components: a context expression and a
member expression. The value of context expression is the parent node of the
member expression.
*/

class NLANG_COMPILER_API SnMemberExpr : public SnCompoundFieldExpr
{
	friend class NameResolver;
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_MemberExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnMemberExpr(SnExpression *pOuter, SnIdentifierExpr *pInner,
		const ISourceLocation &loc);

	SnMemberExpr(SnExpression *pOuter, SnInvokeExpr *pInner,
		const ISourceLocation &loc);

	//Get the context expression.
	SnExpression *Outer() const
	{
		return m_pOuter;
	}

	//Get the member expression of the outer.
	SnFieldExpr *Inner() const
	{
		return m_pInner;
	}

	void Accept(nlang::ISyntaxNodeVisitor&) override;

	std::string ToString() const override;

	/*
	Is the expression a name expression?
	In text source code, a name expression is ether an identifier or an
	identifier list concatenated by dots('.'). For example, "foo", "foo.bar",
	"MyClass.myProp.value", etc.
	*/
	bool IsNameExpr() const;

	static FieldChecker& NameExprChecker();

	static FieldChecker & InvokeExprChecker();
private:
	void Init();
	SnExpression *m_pOuter;
	SnFieldExpr *m_pInner;
};

class BuildEnvironment;

/*
Type name expression.
A type name expression construct from a field name expression.
*/
class NLANG_COMPILER_API SnNameExpr : public SnCompoundFieldExpr
{
	friend class NameResolver;
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_NameExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	static SnNameExpr s_Invalid;
public:
	explicit SnNameExpr(RnField &);

	SnNameExpr(SnIdentifierExpr *pExpr, const ISourceLocation &);

	//Construct from a member expression.
	//The expression must be a name expression. \see SnMemberExpr::IsNameExpr().
	SnNameExpr(SnMemberExpr *pExpr, const ISourceLocation &);

	//Get the source codes expression for this type name.
	SnFieldExpr *Expr() const
	{
		return m_pExpr;
	}

	//Get the runtime field from importing.
	RnField *ImportedField() const
	{
		return m_pImportedField;
	}

	const std::string &MetaName() const
	{
		static const std::string empty;
		if (!m_pExpr || !m_pExpr->Field())
			return empty;
		return m_pExpr->Field()->MetaName();
	}

	void Accept(nlang::ISyntaxNodeVisitor &) override;

	std::string ToString() const override;
private:
	SnFieldExpr *m_pExpr;
	RnField *m_pImportedField;
};

//Array type expression (e.g. "int[]").
//Constructed from an element type expression. The element type may itself
//be an array type, naturally supporting multi-dimensional arrays.
class NLANG_COMPILER_API SnArrayTypeExpr : public SnCompoundFieldExpr
{
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_ArrayTypeExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnArrayTypeExpr(SnFieldExpr *pElemType, const ISourceLocation &loc);

	SnFieldExpr *ElementType() const
	{
		return m_pElementType;
	}

	bool IsArrayType() const override
	{
		return true;
	}

	void Accept(ISyntaxNodeVisitor &) override;

	std::string ToString() const override;
private:
	SnFieldExpr *m_pElementType;
};

//Type cast expression syntax node.
class NLANG_COMPILER_API SnCastExpr : public SnCompoundPlainExpr
{
	friend class NameResolver;
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_CastExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnCastExpr(SnExpression *pSource, TypeCastInfo &, const ISourceLocation &);

	//Get the expression to be cast from.
	SnExpression *Source() const
	{
		return m_pSource;
	}

	//Get the target type to cast to.
	SnField *Target() const
	{
		return m_pTarget;
	}

	//Get the name expression of the target type.
	SnNameExpr *TargetName() const
	{
		return m_pTargetName;
	}

	TypeCastKind CastKind() const
	{
		return m_CastKind;
	}

	virtual bool IsDataExpr() const override;

	void Accept(nlang::ISyntaxNodeVisitor &) override;

	std::string ToString() const override;
private:
	SnExpression *m_pSource;
	SnField *m_pTarget;
	SnNameExpr *m_pTargetName;
	TypeCastKind m_CastKind;
};

//Binary/unary operator expression.
//Reference: EN's OperatorExpr (SeExpressions.h:377).
class NLANG_COMPILER_API SnBinaryExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	enum Operator
	{
		OP_Add, OP_Sub, OP_Mul, OP_Div, OP_Mod,
		OP_Less, OP_LessEqual, OP_Greater, OP_GreaterEqual,
		OP_Equal, OP_NotEqual,
		OP_LogicalAnd, OP_LogicalOr,
		OP_Neg, OP_LogicalNot,
	};

	static const NodeKind	s_Kind			= NK_BinaryExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	//Binary operator: left op right
	SnBinaryExpr(Operator op, SnExpression *pLeft, SnExpression *pRight,
		const ISourceLocation &loc);

	//Unary operator: op left
	SnBinaryExpr(Operator op, SnExpression *pLeft,
		const ISourceLocation &loc);

	Operator Op() const { return m_op; }
	SnExpression *Left() const { return m_pLeft; }
	SnExpression *Right() const { return m_pRight; }

	bool IsDataExpr() const override;
	void Accept(nlang::ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	Operator m_op;
	SnExpression *m_pLeft;
	SnExpression *m_pRight;
};

class SnClassDecl;

//The "new" expression for object instantiation.
class NLANG_COMPILER_API SnNewExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_NewExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnNewExpr(SnFieldExpr *pClassName, UniquePtrList<SnExpression> upArgs,
		const ISourceLocation &loc);

	~SnNewExpr() override;

	SnFieldExpr *ClassName() const { return m_pClassName; }
	SnExpressionList &Args() const { return *m_pArgs; }
	SnClassDecl *ClassDecl() const { return m_pClassDecl; }
	void ClassDecl(SnClassDecl *pClass) { m_pClassDecl = pClass; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	SnFieldExpr *m_pClassName;
	SnExpressionList *m_pArgs;
	SnClassDecl *m_pClassDecl;
};

//The "this" expression — refers to the current object in a method.
class NLANG_COMPILER_API SnThisExpr : public SnFieldExpr
{
	typedef SnFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_ThisExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	explicit SnThisExpr(const ISourceLocation &loc);

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
};

//Array subscript access expression (e.g. arr[i]).
class NLANG_COMPILER_API SnSubscriptExpr : public SnCompoundFieldExpr
{
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_SubscriptExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnSubscriptExpr(SnExpression *pArray, SnExpression *pIndex,
		const ISourceLocation &loc);

	SnExpression *Array() const { return m_pArray; }
	SnExpression *Index() const { return m_pIndex; }

	bool IsDataExpr() const override { return true; }
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	SnExpression *m_pArray;
	SnExpression *m_pIndex;
};

//Array allocation expression (e.g. new int[10]).
class NLANG_COMPILER_API SnNewArrayExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_NewArrayExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnNewArrayExpr(SnFieldExpr *pElemType, SnExpression *pSize,
		const ISourceLocation &loc);

	~SnNewArrayExpr() override;

	SnFieldExpr *ElementType() const { return m_pElemType; }
	SnExpression *Size() const { return m_pSize; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	SnFieldExpr *m_pElemType;
	SnExpression *m_pSize;
};

} //namespace nlang
