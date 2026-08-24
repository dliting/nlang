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
#include <vector>
#include <string>

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

//Phase 8e-6: one entry in an init list `{...}`.
//KeyKind==String → dict entry (key is a string literal value).
//KeyKind==Identifier → struct/class field (key is a field name).
//Array-form init lists `[...]` do not carry entries of this struct;
//they use the plain element list directly via the Entries() vector.
//Per-entry source location can be retrieved from pValue->Location()
//when needed for diagnostics.
struct InitEntry
{
	enum class KeyKind { None, String, Identifier };
	KeyKind          keyKind = KeyKind::None;
	std::string      keyStr;
	SnExpression    *pValue = nullptr;
};

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

//Phase 9b: interpolated string expression "hello ${expr} world".
//Implemented as OP_Add binary tree — see BuildStringExpr in nlang.y.
//No dedicated AST node required.

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
class SnFormalParam;  //forward decl (defined in SnData.h); needed for FormalBinding

//Phase 9c: result of binding a call-site argument list to a callee's
//formal parameter list. Each entry describes how formal[i] is satisfied.
//Produced by ExprResolveAccessor::FindFuncByInvoke; consumed by VmBackend
//EmitCallArgs to drive per-formal evaluation (including defaults).
struct FormalBinding
{
	enum Kind
	{
		B_Positional,  //caller provided this formal via positional arg
		B_Named,       //caller provided this formal via name = expr
		B_Default      //caller omitted; use formal's default expression
	};
	Kind            kind;
	SnExpression   *pCallerExpr;  //non-null for B_Positional / B_Named
	SnFormalParam  *pFormal;      //always non-null
	bool            bIsOut = false; //Phase 9e: caller wrote `out ident`
	                                //and the formal is an out parameter
};

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

	//Phase 9c: per-formal binding decisions. Empty for invokes that did not
	//go through Phase 9c resolver path (codegen treats empty as "all positional"
	//for backward compatibility).
	const std::vector<FormalBinding>& Bindings() const { return m_Bindings; }
	void SetBindings(std::vector<FormalBinding>&& v) { m_Bindings = std::move(v); }
	bool HasBindings() const { return !m_Bindings.empty(); }
private:
	std::unique_ptr<std::string> m_upCalleeName;
	SnExpressionList* m_pParams;
	std::vector<FormalBinding> m_Bindings;
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

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
private:
	SnFieldExpr *m_pElementType;
};

//Phase 8e-3: Generic type expression (e.g. "List<int>", "Dict<K,V>").
//Constructed from a base type name and a list of type arguments. The base
//must resolve to a built-in generic class; user-defined generics are not
//supported. ExprResolver mints a per-instantiation synthetic SnClassDecl
//(e.g. "List<int>") that carries substituted method signatures while
//sharing a single backing CompiledClass at runtime (erasure model).
class NLANG_COMPILER_API SnGenericTypeExpr : public SnCompoundFieldExpr
{
	typedef SnCompoundFieldExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_GenericTypeExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnGenericTypeExpr(SnFieldExpr *pBase,
		std::vector<SnFieldExpr*> *pTypeArgs,
		const ISourceLocation &loc);

	//The base type name expression (e.g. "List").
	SnFieldExpr *Base() const { return m_pBase; }

	//The type arguments (e.g. [int] or [Point]).
	const std::vector<SnFieldExpr*> &TypeArgs() const
	{
		return *m_upTypeArgs;
	}

	void Accept(ISyntaxNodeVisitor &) override;

	std::string ToString() const override;

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
private:
	SnFieldExpr							*m_pBase;
	std::unique_ptr<std::vector<SnFieldExpr*>>	m_upTypeArgs;
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

//Phase 8e-1.5: `expr as T` runtime-checked cast expression.
//Created by the parser. ExprResolver fills m_pResolvedTarget and m_CastKind.
//VmBackend emits OP_Unbox (primitive unbox) or OP_CheckCast (class downcast)
//based on the resolved CastKind.
//Deviation from the original (T)expr plan: LALR(1) could not distinguish
//(TypeName)expr from (expr) at parse time without a symbol-table lexer hack.
//The `as` keyword is unambiguous and matches C#/TypeScript/Kotlin syntax.
class NLANG_COMPILER_API SnAsExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_AsExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnAsExpr(SnExpression *pOperand, SnNameExpr *pTargetType,
		const ISourceLocation &loc)
		: Super_(s_Kind, loc), m_pOperand(pOperand), m_pTargetType(pTargetType),
		  m_pResolvedTarget(nullptr), m_CastKind(TCK_None)
	{
		assert(pOperand);
		assert(pTargetType);
		AddChild(m_pOperand);
		AddChild(m_pTargetType);
	}

	SnExpression *Operand() const { return m_pOperand; }
	//The as-target type slot. Widened from SnNameExpr* in Phase 13 so the
	//alias pre-pass can splice a cloned generic/array alias RHS in place of
	//the parsed name; resolution goes through the generic Accept dispatch.
	SnFieldExpr *TargetType() const { return m_pTargetType; }

	//Filled by ExprResolver.Access(SnAsExpr&).
	SnField *ResolvedTarget() const { return m_pResolvedTarget; }
	TypeCastKind CastKind() const { return m_CastKind; }
	void SetResolved(SnField *pTarget, TypeCastKind kind)
	{
		m_pResolvedTarget = pTarget;
		m_CastKind = kind;
		AddFlags(NF_Resolved);
	}

	virtual bool IsDataExpr() const override;
	void Accept(nlang::ISyntaxNodeVisitor &) override;
	std::string ToString() const override;

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
private:
	SnExpression *m_pOperand;
	SnFieldExpr	*m_pTargetType;
	SnField		*m_pResolvedTarget;
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
	//NOTE: Args() is a view over ALL child nodes. The ctor AddChild's the
	//class-name expression AFTER the ctor args, so the view's LAST element
	//is the class name (a NameExpr for `new Foo`, a GenericTypeExpr for
	//`new List<int>`). Consumers must skip it by identity:
	//`if (&param == ClassName()) continue;`
	SnExpressionList &Args() const { return *m_pArgs; }
	SnClassDecl *ClassDecl() const { return m_pClassDecl; }
	void ClassDecl(SnClassDecl *pClass) { m_pClassDecl = pClass; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
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

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
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

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
private:
	SnFieldExpr *m_pElemType;
	SnExpression *m_pSize;
};

//Phase 8e-6: Collection initializer literal.
//  - Bare array/list form:    [e1, e2, ...]            (LHS-inferred)
//  - Explicit dict/struct:    new Type{ k: v, ... }     (Type mandatory)
//Bare `{...}` form was dropped (LALR(1) conflict with Paragraph block).
//Resolver dispatches on resolved target type. Codegen lowers via
//existing alloc/new/method-call/field-store opcodes (no new opcode).
class NLANG_COMPILER_API SnInitListExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_InitListExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	//pExplicitType is nullptr for the bare form. For `new Type{...}`,
	//pExplicitType carries the Type expression (NameExpr/GenericTypeExpr).
	//isArrayForm distinguishes `[...]` (true) from `{...}` (false).
	//For array form, entries' keyKind is None (only values are used).
	SnInitListExpr(SnFieldExpr *pExplicitType,
		std::vector<InitEntry> entries, bool isArrayForm,
		const ISourceLocation &loc);

	~SnInitListExpr() override;

	SnFieldExpr *ExplicitType() const { return m_pExplicitType; }
	const std::vector<InitEntry> &Entries() const { return m_entries; }
	bool IsArrayForm() const { return m_isArrayForm; }

	//For the bare `[...]` form, the parent context (AssignStmt resolver)
	//sets the inferred target field from the LHS before this node resolves.
	//Nullptr until the parent populates it. The resolver uses this only
	//when ExplicitType is null.
	SnField *InferredTarget() const { return m_pInferredTarget; }
	void InferredTarget(SnField *pT) { m_pInferredTarget = pT; }

	//Set by resolver when the target (explicit or inferred) is an array
	//type. NLang represents `int[]` as a variable with element-type field
	//and IsArrayType()==true; there is no standalone "array of T" type
	//object. This flag preserves array-ness through to codegen.
	bool TargetIsArray() const { return m_bTargetIsArray; }
	void TargetIsArray(bool b) { m_bTargetIsArray = b; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
private:
	SnFieldExpr                 *m_pExplicitType;        //nullptr for bare form
	std::vector<InitEntry>       m_entries;              //empty allowed
	bool                         m_isArrayForm;
	SnField                     *m_pInferredTarget = nullptr;  //parent-set, bare form
	bool                         m_bTargetIsArray = false;     //resolver-set
};

//Phase 9c: named argument expression `name = expr` at call sites.
//Wraps an inner expression with a parameter name. ExprResolver extracts the
//name and binds to the matching formal parameter; codegen treats the node
//as transparent (uses Inner() directly via the resolved BindingMap).
class NLANG_COMPILER_API SnNamedArgExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_NamedArgExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnNamedArgExpr(std::string *pName, SnExpression *pInner,
		const ISourceLocation &loc)
		: Super_(s_Kind, loc), m_pInner(pInner)
	{
		assert(pName);
		assert(pInner);
		m_upName.reset(pName);
		AddChild(m_pInner);
	}

	const std::string &Name() const { return *m_upName; }
	SnExpression *Inner() const { return m_pInner; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	std::unique_ptr<std::string> m_upName;
	SnExpression                 *m_pInner;
};

//Phase 9e: out argument expression `out ident` at call sites.
//Wraps the inner identifier; ExprResolver validates the inner refers to a
//local/formal slot and requires the matched formal to be an out parameter.
//TryBindInvoke unwraps the node into FormalBinding (pCallerExpr = Inner())
//with bIsOut set, so codegen emits the inner read into the staging slot
//and records a writeback pair for OP_CallFuncOut/OP_CallMethodDirectOut.
class NLANG_COMPILER_API SnOutArgExpr : public SnCompoundPlainExpr
{
	typedef SnCompoundPlainExpr Super_;
public:
	static const NodeKind	s_Kind			= NK_OutArgExpr;
	static const NodeBits	s_DefaultFlags	= NF_Expression;
public:
	SnOutArgExpr(SnExpression *pInner, const ISourceLocation &loc)
		: Super_(s_Kind, loc), m_pInner(pInner)
	{
		assert(pInner);
		AddChild(m_pInner);
	}

	SnExpression *Inner() const { return m_pInner; }

	bool IsDataExpr() const override;
	void Accept(ISyntaxNodeVisitor &) override;
	std::string ToString() const override;
private:
	SnExpression                 *m_pInner;
};

} //namespace nlang
