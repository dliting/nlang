#pragma once
#include "SnExpressions.h"
#include "SyntaxNodeVisitor.h"
#include <nlang/runtime/Flagable.h>

namespace nlang
{

enum FindFuncResult
{
	//The function name is not found.
	FFR_FuncNameNotFound,
	//The function is found but the parameter types are not compatible.
	FFR_Incompatible,
	//The function is found and the parameter types are compatible but not 
	//exactly matched, automatic type cast is needed.
	FFR_ApproximateMatch,
	//The function is found and exactly matched, no type cast is needed.
	FFR_ExactMatch
};

enum ExprResolveFlag : uint8
{
	ERF_None = 0,
	//Don't search in using namespaces.
	ERF_IgnoreUsings = 1,
	//Don't search in remote ancestors.
	ERF_SearchInParentOnly = 2,
};

typedef uint8 ExprResolveFlagSet;

/*
The syntax node accessor for expression resolving.
This class should be used with \a ExprResolver.
*/
class ExprResolveAccessor : public ChildNodeListController, 
	public Flagable<uint8>
{
	friend class ExprResolver;
public:
	void Access(SnLiteralExpr &);

	void Access(SnNameExpr &);

	void Access(SnArrayTypeExpr &);

	void Access(SnIdentifierExpr &);

	void Access(SnInvokeExpr &);

	void Access(SnMemberExpr &);

	void Access(SnCastExpr &);

	//Resolve binary/unary operator expression.
	void Access(SnBinaryExpr &);

	//Resolve new expression.
	void Access(SnNewExpr &);
	//Resolve new array expression.
	void Access(SnNewArrayExpr &);

	//Resolve subscript expression.
	void Access(SnSubscriptExpr &);

	//Resolve this expression.
	void Access(SnThisExpr &);

	//Resolve class declaration.
	void Access(SnClassDecl &);

	//Resolve class field.
	void Access(SnClassField &);

	//Default action.
	void Access(SyntaxNode &)
	{
		assert(false && "invalid expression.");
	}
private:
	explicit ExprResolveAccessor(BuildEnvironment &env) : m_Env(env),
		m_pVisitor(nullptr), m_pContext(nullptr), m_pAccessor(nullptr)
	{
	}

	bool ResolveExpressionList(SnExpressionList &exprs);

	void ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField);

	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor, ExprResolveFlagSet flags);

	SnField *FindFieldInUsings(std::string &sName, const UsingList &usings,
		const SnField & accessor);

	/*
	Find the best function declaration matched witch an invoke expression.
	Precondition: the parameters in the invoke expression are all resolved.
	*/
	FindFuncResult FindFuncByInvoke(SnFunction *&pFunc, SnInvokeExpr &invoke);

	/*
	Calculate the type "distance" from concrete parameters to formal
	parameters.
	\return If the distance between any of the concrete parameters and its
	corresponding formal one is negative, return -1; else return the sum of
	all distances of parameters.
	*/
	int CalcDistanceOfParams(const SnExpressionList &concretParams,
		const SnFunction::ParamList &formalParams) const;

	/*
	Calculate the type "distance" from source type to target type.
	\return
	1) If the source type can be implicitly covert to target type:
	1.1)If the source type and the target type are primitive types, it returns
	abs(source.Kind() - Target.Kind()).
	1.2)If the source type is primitive type, the target type is Object, it
	returns PRIMITIVE_TYPE_COUNT.
	1.3)If the source type and the target type are classes or interfaces, it
	returns the depth of inheritance.
	1.4)If the source type is enumerator and the target type is not enumerator,
	the source type will be convert to it primitive type firstly, and use the
	above rules to calculate the distance.
	2) If the source type can not be implicitly covert to target type, it
	returns	-1.
	*/
	int CalcTypeDistance(const SnField &source,
		const SnField &target) const;

	void FixupParamTypes(SnInvokeExpr &invoke,
		SnFunction::ParamList &formalParams);

	/*
	Create a cast expression and replace the exist expression by cast
	information.
	If no cast is needed or the source cannot be automatically cast to the 
	target type, nothing will be done.
	\param iSrcExpr The position where the original expression locates.
	\param castInfo The cast information.
	\return Return false if the source expression can not be automatically cast
	to the target type, else return true.
	*/
	bool FixupExprType(NodeIterator &iSrcExpr, TypeCastInfo &castInfo);

	ISyntaxNodeVisitor *m_pVisitor;
	SyntaxNode *m_pContext;
	SnField *m_pAccessor;
	BuildEnvironment &m_Env;
};

//The helper class to resolve expressions.
class ExprResolver : public Flagable<uint8>
{
public:
	explicit ExprResolver(BuildEnvironment &env) : 
		m_Accessor(env), m_Visitor(m_Accessor, NVK_CustomTraverse)
	{
		m_Accessor.m_pVisitor = &m_Visitor;
	}

	bool Resolve(SnExpression &sn, SyntaxNode &context, SnField &accessor,
		ExprResolveFlagSet flags = ERF_None)
	{
		m_Accessor.m_pContext = &context;
		m_Accessor.m_pAccessor = &accessor;
		m_Accessor.Flags(flags);
		sn.Accept(m_Visitor);
		return sn.IsResolved();
	}

	//Resolve the child data type nodes in a specified field.
	//\note The data types in expressions will not be resolved.
	bool ResolveDataTypes(SnField &, SnField &outerType);

	/*
	Create a cast expression and replace the exist expression by cast
	information.
	If no cast is needed or the source cannot be automatically cast to the target
	type, nothing will be done.
	\param iSrcExpr The iterator reference where the original expression 
	locates.
		This reference will be replace with the inserted iterator to the newly
	created	cast expression.
	\param castInfo The cast information.
	\return Return false if the source expression can not be automatically cast
	to the target type, else return true.
	*/
	bool FixupExprType(NodeIterator &iSrcExpr, TypeCastInfo &castInfo)
	{
		return m_Accessor.FixupExprType(iSrcExpr, castInfo);
	}

	void ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
	{
		m_Accessor.ResolveFieldExprAs(expr, pField);
	}
private:

	bool ResolveDataType(SnFieldExpr & typeExpr, SnField & outerType);

	bool ResolveChildFields(SnField & sn);
	ExprResolveAccessor m_Accessor;
	SyntaxNodeVisitor<ExprResolveAccessor> m_Visitor;
};

} //namespace nlang