/*---
    StatementResolverStoreGates.cpp — 赋值目标守卫族：const 局部、方法调用结果、创建孪生与字符串基的赋值拒绝。
    从 StatementResolverAssign.cpp 抽取（0.9.0 尺寸守卫解除，零行为变化）。
---*/
#include "StatementResolver.h"
#include <nlang/compiler/SnMisc.h>

namespace nlang
{

//Phase 9a: reject assignment to a const local (identifier targets only;
//the const-init decomposition sets NF_Const AFTER resolving the
//initializer AssignStmt, so initializer writes correctly pass).
//0.9.10 D3.5: interface constants are folded values with no location —
//a member-expression store targeting one is rejected here too.
void StatementResolveAccessor::RejectConstStoreTarget(SnExpression &left,
	const ISourceLocation *pLoc)
{
	if (left.Kind() == NK_MemberExpr)
	{
		auto *pMemberField = static_cast<SnMemberExpr &>(left).Field();
		if (pMemberField && IsInterfaceConstantField(*pMemberField))
			m_Env.Log(CLL_Error, pLoc,
				"\"%s\" is a constant and cannot be assigned.",
				pMemberField->Name().c_str());
		return;
	}
	if (left.Kind() != NK_IdentifierExpr)
		return;
	auto* pLeftField = static_cast<SnIdentifierExpr&>(left).Field();
	if (pLeftField && pLeftField->ContainFlags(NF_Const))
		m_Env.Log(CLL_Error, pLoc,
			"cannot assign to const local \"%s\".",
			pLeftField->Name().c_str());
}

//A method call LHS (`obj.f() = v`, `obj.f() += v`) parses as a store
//statement — the grammar's MemberExpr admits `Expression '.' InvokeExpr`
//because that is also how every method call is spelled — but the call
//result is a value, not a location. Without this gate the statement
//resolves quietly (the method's return type reads as the target type)
//and codegen's field-store paths silently emit nothing for it.
void StatementResolveAccessor::RejectMethodCallStoreTarget(
	SnExpression &left, const ISourceLocation *pLoc)
{
	if (left.Kind() != NK_MemberExpr)
		return;
	if (static_cast<SnMemberExpr&>(left).Inner()->Kind()
		== NK_IdentifierExpr)
		return;
	m_Env.Log(CLL_Error, pLoc,
		"cannot assign to the result of a method call.");
}

//0.9.0: a creation twin in subscript-store position (`int[3] = v`) has
//a TYPE expression where the grammar expects a location — the value
//producer has nothing to store into. Without this gate the statement
//resolves quietly and codegen dies on the bare type identifier
//("identifier has no codegen binding"). The grammar gives the store
//base a bare identifier, so the type-shape check is a plain identifier
//whose field is a type; value bases (local, member chain, invoke
//result) never carry a type field this way. True = rejected (the
//caller stops).
bool StatementResolveAccessor::RejectCreationStoreTarget(
	SnExpression &base, const ISourceLocation *pLoc)
{
	if (base.Kind() != NK_IdentifierExpr || !base.IsResolved())
		return false;
	auto *pField = static_cast<SnIdentifierExpr &>(base).Field();
	if (!pField || !pField->IsTypeField())
		return false;
	m_Env.Log(CLL_Error, pLoc, "cannot assign to an array creation.");
	return true;
}

//0.8.4/0.9.0: the compound-assign subscript LHS gates. A string base
//(`s[0] += v`) has no store semantics — the same gate as the plain
//subscript store (RejectStringBase). A creation twin target
//(`int[2] += v`, `P[2] += v` through an alias) was consumed as a
//creation by the expression resolver — the NewArrayElem annotation
//marks it, and a creation has no location. True = rejected (the
//caller stops).
bool StatementResolveAccessor::RejectCompoundSubscriptTarget(
	SnExpression &left, const ISourceLocation *pLoc)
{
	if (left.Kind() != NK_SubscriptExpr)
		return false;
	auto& sub = static_cast<SnSubscriptExpr&>(left);
	if (sub.NewArrayElem())
	{
		m_Env.Log(CLL_Error, pLoc, "cannot assign to an array creation.");
		return true;
	}
	return sub.Array() && RejectStringBase(*sub.Array(), pLoc);
}

//0.7.3 B D3: write-path twin of the read arm in ExprResolver — a string
//base has no subscript store semantics. Before this arm the store fell
//through the container path with a null element type (strings are not
//generic instantiations) and failed only at runtime ("null array
//access"). 0.8.4: shared with the compound-assign subscript LHS
//(`s[0] += v`) — the same base has no store semantics there.
bool StatementResolveAccessor::RejectStringBase(SnExpression &base,
	const ISourceLocation *pLoc)
{
	if (base.IsResolved()
		&& base.EvalDataType()
		&& base.EvalDataType()->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, pLoc,
			"string does not support subscript access.");
		return true;
	}
	return false;
}

} //namespace nlang
