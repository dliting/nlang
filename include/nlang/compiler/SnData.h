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

//Phase 9c: sanity ceiling on a function's formal parameter count.
//
//The VM frame layout now sizes callParamBase dynamically per caller
//(based on the maximum callee formal count seen in the caller's body),
//so there is no fixed 8-slot cap. This constant serves as a sanity
//ceiling to prevent unreasonably large frames. Exceeding it is a
//declaration-time error in StatementResolver.Access(SnFunction).
static constexpr size_t kMaxFuncParams = 64;

//The base class of data field syntax nodes.
class NLANG_COMPILER_API SnDataField : public SnField
{
	friend class DataGenerateAccessor;
	typedef SnField Super_;
public:
	//Construct from a parser.
	SnDataField(NodeKind k, FieldAccessType at, NodeBits flags,
		SnFieldExpr *pType, std::string *pName, SnExpression *pDefault,
		const ISourceLocation &loc);

	//Construct from an runtime type.
	explicit SnDataField(RnDataField &);

	~SnDataField() override;

	//Get the value type of the parameter.
	SnFieldExpr *Type() const
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

	bool IsArrayType() const override;

	std::string ToString() const override;

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	void Init();
	SnFieldExpr *m_pType;
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
	SnFormalParam(NodeBits flags, SnFieldExpr *pType, std::string *pName,
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
	SnFunction(FieldAccessType, NodeBits flags, SnFieldExpr *pReturnType,
		std::string *pName, UniquePtrList<SnFormalParam> upParams,
		const ISourceLocation &loc);

	//Create from an existing meta function.
	explicit SnFunction(RnFunction &);

	//Get the return type of this function.
	SnFieldExpr *ReturnType() const
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

	//Does this function have an implementation a call can run? True when
	//it carries a body (source), the native flag (a host-table binding),
	//or the imported flag (a foreign unit's registration). A body-less,
	//non-native, non-imported declaration — the abstract shape a class or
	//struct member takes from a trailing ';' — has nothing to run. This is
	//the single definition of the constructor-registration predicate: the
	//backend's VmBackend::IsRegisteredCtor delegates here and the
	//resolver's constructor-candidate filter (ExprResolverNew.cpp) calls
	//it, so the two layers never diverge — a divergent filter splits the
	//frame-walker claim from the codegen claim (see VmBackend.h).
	bool HasImplementation() const
	{
		return IsImported() || Body() || ContainFlags(NF_Native);
	}

#ifdef NLANG_ENABLE_LLVM
	llvm::Function *MetaFunc() const;
#endif

	SnField *FindField(const std::string& sName) const override;

	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &v) override;

	std::string ToString() const override;

	bool ConflictedWith(const SnField &other) const override;

	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;

	virtual SnField *EvalDataType() const override;

	//Phase 8e-3: marks a synthetic method on a built-in generic class
	//(e.g. List<int>.Add). When true, VmBackend emits OP_Box before
	//OP_CallMethod for primitive-typed arguments — the runtime side
	//operates on Object/heap-idx storage (erasure model).
	bool m_bErasedGeneric = false;
	//0.10.14: the callee's index in the PRODUCER's own function table,
	//minted onto every function stub (all mint sites pass it). The
	//namespace branch of the cross-unit slot record (free functions,
	//interface statics, enum methods) carries it as
	//SymbolImport::ownerOrdinal so nlink single-selects the record
	//instead of scanning by name+arity — the same authority+recheck
	//model as class/struct member ordinals. 0xFFFF = not minted from a
	//producer table index (own declarations, or a producer table
	//beyond the 16-bit index space).
	uint16_t m_iProducerFuncIdx = 0xFFFF;
private:
	void Init();
	SnFieldExpr *m_pReturnType;
	std::unique_ptr<ParamList> m_upParams;
	SnParagraph *m_pBody;
};

} //namespace nlang