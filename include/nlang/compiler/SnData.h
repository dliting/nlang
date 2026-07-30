#pragma once
#include "SyntaxNode.h"
#include "SnExpressions.h"
#include "SnStatements.h"
#include <nlang/runtime/RnData.h>
#include <memory>

//Foreword declarations.
#ifdef NLANG_ENABLE_LLVM
namespace llvm
{
	class Function;
} //namespace llvm
#endif

namespace nlang
{

//The base class of data field syntax nodes.
class NLANG_COMPILER_API SnDataField : public SnField
{
	friend class DataGenerateAccessor;
	typedef SnField Super_;
public:
	//Construct from a parser.
	SnDataField(NodeKind k, FieldAccessType at, NodeBits flags, 
		SnNameExpr *pType, std::string *pName, SnExpression *pDefault,
		const ISourceLocation &loc);

	//Construct from an runtime type.
	explicit SnDataField(RnDataField &);

	~SnDataField() override;

	//Get the value type of the parameter.
	SnNameExpr *Type() const
	{
		return m_pType;
	}

	//Get the expression of the default value.
	SnExpression *Value() const
	{
		return m_pValue;
	}

	SnField *FindField(const std::string& sName) const override;

	SnField *EvalDataType() const override;

	std::string ToString() const override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	void Init();
	SnNameExpr *m_pType;
	SnExpression *m_pValue;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

//The syntax node of a formal parameter of a function.
class NLANG_COMPILER_API SnFormalParam : public SnDataField
{
	typedef SnDataField Super_;
public:
	typedef RnFormalParam RuntimeType;
public:
	//Construct from a parser.
	SnFormalParam(NodeBits flags, SnNameExpr *pType, std::string *pName, 
		SnExpression *pDefault,	const ISourceLocation &loc);

	//Construct from an import module.
	explicit SnFormalParam(RnFormalParam &);

	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor&) override;
};

//A syntax node of function declaration.
class NLANG_COMPILER_API SnFunction : public SnCompoundField
{
	friend class DataGenerateAccessor;
	typedef SnCompoundField Super_;
public:
	typedef ChildFieldList<SnFormalParam> ParamList;
	typedef RnFunction RuntimeType;
public:
	//Create from parsing information.
	SnFunction(FieldAccessType, NodeBits flags, SnNameExpr *pReturnType,
		std::string *pName, UniquePtrList<SnFormalParam> upParams, 
		const ISourceLocation &loc);

	//Create from an existing meta function.
	explicit SnFunction(RnFunction &);

	//Get the return type of this function.
	SnNameExpr *ReturnType() const
	{
		return m_pReturnType;
	}

	//Get the parameters.
	//@{
	const ParamList &Params() const
	{
		return *m_upParams;
	}

	ParamList &Params()
	{
		return *m_upParams;
	}
	//@}

	//Set the statements.
	void Body(SnParagraph *pBody)
	{
		ResetChild(m_pBody, pBody);
	}

	//Get the statements in this function.
	SnParagraph *Body() const
	{
		return m_pBody;
	}

	//Is the return type not "void"?
	bool HasReturn() const
	{
		return m_pReturnType != nullptr;
	}

#ifdef NLANG_ENABLE_LLVM
	llvm::Function *MetaFunc() const;
#endif

	SnField *FindField(const std::string& sName) const override;

	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &v) override;

	std::string ToString() const override;

	bool ConflictedWith(const SnField &other) const override;

	virtual SnField *EvalDataType() const override;
private:
	void Init();
	SnNameExpr *m_pReturnType;
	std::unique_ptr<ParamList> m_upParams;
	SnParagraph *m_pBody;
};

} //namespace nlang