/*---
    StatementResolverStoreGates.cpp — 赋值目标守卫族：const 局部、方法调用结果、创建孪生与字符串基的赋值拒绝。
    从 StatementResolverAssign.cpp 抽取（0.9.0 尺寸守卫解除，零行为变化）。
---*/
#include "StatementResolver.h"
#include <nlang/compiler/SnMisc.h>

namespace nlang
{

//v0.12.4 D1/D4: the sealed-store verdict for a field target. True when
//the field is declared by a const class/struct and the write is not
//inside that declaration's own constructor. Locals and parameters are
//never sealed here — the variable slot itself stays mutable (a const
//struct local may be rebound to a fresh value; only its fields are
//sealed through the member arm).
//D3: the construction exemption is scoped to the type that DECLARES the
//field — its own constructors may seed it (injected field initializers
//become ctor statements, so they ride the same exemption). A subclass
//constructor writing an inherited field stays rejected: the subclass
//never owns that slot, and per the D1 inheritance ruling the subclass
//is const itself, so allowing the write would bypass the base's seeding
//discipline. The first NK_Function up the parent chain is the enclosing
//function (NLang has no nested functions).
bool StatementResolveAccessor::IsSealedFieldWrite(SnExpression &left,
	SnField &field)
{
	auto *pOwner = field.Parent();
	if (!pOwner)
		return false;
	bool bConstOwner;
	if (pOwner->Kind() == NK_ClassDecl)
		bConstOwner = static_cast<SnClassDecl *>(pOwner)
			->IsConstAggregate();
	else if (pOwner->Kind() == NK_StructDecl)
		bConstOwner = static_cast<SnStructDecl *>(pOwner)
			->IsConstAggregate();
	else
		return false;
	if (!bConstOwner)
		return false;
	for (SyntaxNode *pScope = left.Parent(); pScope;
		pScope = pScope->Parent())
	{
		if (pScope->Kind() != NK_Function)
			continue;
		auto &fn = static_cast<SnFunction &>(*pScope);
		if (!IsConstructorFunction(fn))
			return true;
		return fn.Parent() != field.Parent();
	}
	return true;
}

//v0.12.4 D4: the shared diagnostic of the sealed-field store gates.
void StatementResolveAccessor::RejectSealedFieldStore(SnField &field,
	const ISourceLocation *pLoc)
{
	auto *pOwner = field.Parent();
	if (pOwner->Kind() == NK_StructDecl)
		m_Env.Log(CLL_Error, pLoc,
			"The field \"%s\" is immutable: it belongs to the const "
			"struct \"%s\" and only that struct's constructors may "
			"write it.",
			field.Name().c_str(),
			static_cast<SnStructDecl *>(pOwner)->Name().c_str());
	else
		m_Env.Log(CLL_Error, pLoc,
			"The field \"%s\" is immutable: it belongs to the const "
			"class \"%s\" and only that class's constructors may "
			"write it.",
			field.Name().c_str(),
			static_cast<SnClassDecl *>(pOwner)->Name().c_str());
}

//Phase 9a: reject assignment to a const local (identifier targets only;
//the const-init decomposition sets NF_Const AFTER resolving the
//initializer AssignStmt, so initializer writes correctly pass).
//0.9.10 D3.5: interface constants are folded values with no location —
//a member-expression store targeting one is rejected here too.
//v0.12.4 D4: stores into the fields of a const class/struct are
//rejected on both arms — member targets (`c.n = v`, `this.n = v`,
//chained `h.in.n = v`) and implicit-this identifier targets (`n = v`
//inside a method). The only legal writer is the declaring type's own
//constructor (D3).
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
		else if (pMemberField && IsSealedFieldWrite(left, *pMemberField))
			RejectSealedFieldStore(*pMemberField, pLoc);
		return;
	}
	if (left.Kind() != NK_IdentifierExpr)
		return;
	auto* pLeftField = static_cast<SnIdentifierExpr&>(left).Field();
	if (pLeftField && pLeftField->ContainFlags(NF_Const))
		m_Env.Log(CLL_Error, pLoc,
			"cannot assign to const local \"%s\".",
			pLeftField->Name().c_str());
	else if (pLeftField && IsSealedFieldWrite(left, *pLeftField))
		RejectSealedFieldStore(*pLeftField, pLoc);
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

//K3 task 4: the resolved field behind an assign target — identifier
//targets directly, member stores through the already-bound inner
//identifier (the left walk binds `b.m`'s base before the RHS resolves).
//Shared by the init-list and ctor-invoke target propagations below.
static SnField* AssignLeftField(SnAssignStmt &sn)
{
	if (sn.Left()->Kind() == NK_IdentifierExpr)
		return static_cast<SnIdentifierExpr&>(*sn.Left()).Field();
	if (sn.Left()->Kind() == NK_MemberExpr)
	{
		auto* pInner = static_cast<SnMemberExpr&>(*sn.Left()).Inner();
		if (pInner && pInner->Kind() == NK_IdentifierExpr)
			return static_cast<SnIdentifierExpr*>(pInner)->Field();
	}
	return nullptr;
}

//Init lists set their own type and don't go through the cast-info path,
//so an untyped `[]`/`{...}` RHS inherits the LHS field as its target.
//0.9.0: member stores (`b.m = {...}`) read the resolved field the same
//way — the left walk has already bound the member's inner identifier.
void StatementResolveAccessor::PropagateInitListTarget(SnAssignStmt &sn)
{
	auto *pInitList = UntypedInitListOf(*sn.Right());
	if (!pInitList)
		return;
	SnField* pLeftField = AssignLeftField(sn);
	if (pLeftField)
		pInitList->InferredTarget(pLeftField);
}

//K3 task 4: when the RHS is a bare invoke, hand the resolved LHS field
//to the ctor-invoke arm (ResolveDeclaredGenericCtorTarget consumes it
//for `Channel<int> c = Channel(4);`-shaped declared-generic calls).
void StatementResolveAccessor::PropagateCtorInvokeTarget(SnAssignStmt &sn)
{
	if (sn.Right()->Kind() != NK_InvokeExpr)
		return;
	SnField* pLeftField = AssignLeftField(sn);
	if (pLeftField)
		m_ExprResolver.SetCtorInvokeTarget(pLeftField);
}

} //namespace nlang
