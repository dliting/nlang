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

//Array-masquerade predicates, shared with StatementResolver (Phase 12:
//switch-discriminant family gating). EvalDataType() of an array returns
//the ELEMENT type, so Kind()-based checks alone would let array handles
//through. Definitions live in ExprResolver.cpp.
bool IsArrayTypedBase(SnExpression& baseExpr);
bool IsArrayValuedExpr(SnExpression& expr);

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

	//Phase 8e-3: resolve `List<T>` (and future built-in generics).
	//Mints or fetches a cached synthetic SnClassDecl per (base, typeArgs)
	//tuple to preserve pointer identity for CalcTypeDistance.
	void Access(SnGenericTypeExpr &);

	void Access(SnIdentifierExpr &);

	void Access(SnInvokeExpr &);

	//Phase 9c: resolve a named argument `name = expr` at a call site.
	//Resolves the inner expression and propagates its EvalDataType so
	//the parent SnInvokeExpr sees the value's type for overload matching.
	//The name itself is consumed by TryBindInvoke when matching formals.
	void Access(SnNamedArgExpr &);

	//Phase 9e: resolve an out argument `out ident` at a call site.
	//Validates the identifier binds to a caller-frame slot (local var or
	//formal param — both register as NK_FormalParam-kind fields), then
	//propagates the inner's EvalDataType. Consumed by TryBindInvoke, which
	//requires the matched formal to carry NF_Out.
	void Access(SnOutArgExpr &);

	void Access(SnMemberExpr &);

	void Access(SnCastExpr &);

	//Phase 8e-1.5: resolve `expr as T` cast expression.
	void Access(SnAsExpr &);

	//Resolve binary/unary operator expression.
	void Access(SnBinaryExpr &);

	//Resolve new expression.
	void Access(SnNewExpr &);
	//Resolve new array expression.
	void Access(SnNewArrayExpr &);

	//Phase 8e-6: resolve collection initializer (bare [..] or new T{..}).
	//For bare form, uses InferredTarget set by AssignStmt resolver.
	void Access(SnInitListExpr &);

	//Resolve subscript expression.
	void Access(SnSubscriptExpr &);

	//Resolve this expression.
	void Access(SnThisExpr &);

	//Resolve class declaration.
	void Access(SnClassDecl &);

	//Resolve interface declaration (resolve method signatures only).
	void Access(SnInterfaceDecl &);

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

	//Phase 11: resolve a namespace-qualified stdlib call (math.sqrt(x),
	//io.print(s)) against the built-in table in StdLib.h. Called from the
	//top of Access(SnMemberExpr&) — namespace names are reserved and never
	//resolve as fields, so every branch here consumes the expression
	//(resolved or diagnosed); there is no fallback to normal resolution.
	void TryResolveStdLibCall(SnMemberExpr &snMember,
		SnIdentifierExpr &outerId, SnInvokeExpr &invoke);

	void ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField);

	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor, ExprResolveFlagSet flags);

	SnField *FindFieldInUsings(std::string &sName, const UsingList &usings,
		const SnField & accessor);

	/*
	Find the best function declaration matched witch an invoke expression.
	Precondition: the parameters in the invoke expression are all resolved.
	On ExactMatch / ApproximateMatch, outBindings is filled with per-formal
	binding decisions (Phase 9c).
	*/
	FindFuncResult FindFuncByInvoke(SnFunction *&pFunc, SnInvokeExpr &invoke,
		std::vector<FormalBinding> &outBindings);

	/*
	Phase 9c: validate caller-side argument syntax — independent of any
	candidate. Reports specific errors for:
	  - positional argument following a named argument
	  - duplicate name in named arguments (e.g. foo(a=1, a=2))
	Returns true if syntax is well-formed; false after logging the error.
	Called before FindFuncByInvoke so candidate-specific failures don't
	mask these structural errors.
	*/
	bool ValidateInvokeSyntax(const SnInvokeExpr &invoke);

	/*
	Phase 9c: try to bind an invoke's actual arguments to a candidate
	callee's formal parameters. Handles positional args, named args, and
	default param expressions. Returns true if every formal is bound
	(either by caller or by default); false if any required formal is left
	unbound or a caller-side error occurs (positional after named, etc.).
	Does NOT log — caller reports a generic "not compatible" error when
	no candidate matches.
	*/
	bool TryBindInvoke(const SnInvokeExpr &invoke, const SnFunction &callee,
		std::vector<FormalBinding> &outBindings);

	/*
	Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
	entries. B_Default contributes 0. Returns -1 if any bound entry has
	incompatible types.
	*/
	int ComputeBindingDistance(
		const std::vector<FormalBinding> &bindings) const;

	/*
	Phase 9c: apply implicit cast wrappers (SnCastExpr) to caller-side
	expressions in bindings where needed (TCK_Auto / TCK_Box). B_Default
	entries are skipped — their type was validated against the formal at
	declaration time (StatementResolver Step 2).
	*/
	void FixupParamTypesWithBindings(SnInvokeExpr &invoke,
		std::vector<FormalBinding> &bindings);

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