/*---
    StatementResolverTypes.cpp — 类型声明解析：struct/class/interface 成员遍历与 Exception 子类判定。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"

namespace nlang
{

//0.9.0 semantic restriction gate: a variable-style declaration — local,
//formal parameter, catch parameter, loop variable, or class/struct
//field — may not reuse the name of a type visible at that scope. The
//test is the exact complement of the type-position predicate
//(NamesVisibleType): a name spellable as a type here cannot name
//a variable here. This removes at the root the resolution ambiguity
//the TryRebindShadowedTypeName guard patches over; that guard stays
//for the value kinds still allowed to collide (enum members,
//functions in disjoint bare pools, namespaces).
void StatementResolveAccessor::RejectTypeNameVariable(
	const std::string &name, SyntaxNode &scope, const SnField &accessor,
	const ISourceLocation *pLoc)
{
	if (m_ExprResolver.NamesVisibleType(name, scope, accessor))
		m_Env.Log(CLL_Error, pLoc,
			"\"%s\" names a type; it cannot be used as a variable name.",
			name.c_str());
}

void StatementResolveAccessor::Access(SnStructDecl &sn)
{
	assert(m_pVisitor);
	for (auto &field : sn.Members())
	{
		//0.9.0 semantic restriction: struct field names may not reuse
		//visible type names.
		RejectTypeNameVariable(field.Name(), sn, sn, field.Location());
		field.Type()->Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnStructField &sn)
{
}

//Resolve super class reference using ExprResolver (not the
//StatementResolver visitor, which lacks Access(SnNameExpr&)), then
//resolve "implements I1, I2" names into SnInterfaceDecl* pointers.
void StatementResolveAccessor::ResolveClassBases(SnClassDecl &sn)
{
	if (sn.SuperName())
	{
		m_ExprResolver.Resolve(*sn.SuperName(), sn, sn, ERF_None);
		if (sn.SuperName()->IsResolved())
		{
			auto pSuperField = sn.SuperName()->Field();
			if (pSuperField && pSuperField->Kind() == NK_ClassDecl)
				sn.SuperClass(static_cast<SnClassDecl*>(pSuperField));
			else
				m_Env.Log(CLL_Error, sn.SuperName()->Location(),
					"\"%s\" is not a class type.", sn.SuperName()->ToString().c_str());
		}
	}
	for (auto *pName : sn.ImplementsNames())
	{
		m_ExprResolver.Resolve(*pName, sn, sn, ERF_None);
		if (pName->IsResolved())
		{
			auto pField = pName->Field();
			if (pField && pField->Kind() == NK_InterfaceDecl)
				sn.AddImplements(static_cast<SnInterfaceDecl*>(pField));
			else
				m_Env.Log(CLL_Error, pName->Location(),
					"\"%s\" is not an interface type.",
					pName->ToString().c_str());
		}
	}
}

//A method that overrides a parent virtual method
//is also virtual (implicit virtual propagation).
//Check both name and parameter count to avoid false matches.
void StatementResolveAccessor::PropagateVirtualOverrides(SnClassDecl &sn)
{
	auto *pSuper = sn.SuperClass();
	if (!pSuper)
		return;
	for (auto &field : sn.Members())
	{
		if (field.Kind() != NK_Function)
			continue;
		if (field.ContainFlags(NF_Virtual))
			continue;
		auto &childFunc = static_cast<SnFunction&>(field);
		auto *pAncestor = pSuper;
		while (pAncestor)
		{
			auto *pParentMethod = pAncestor->FindField(field.Name());
			if (pParentMethod && pParentMethod->Kind() == NK_Function
				&& pParentMethod->ContainFlags(NF_Virtual))
			{
				auto &parentFunc = static_cast<SnFunction&>(*pParentMethod);
				if (childFunc.Params().size() == parentFunc.Params().size())
				{
					field.AddFlags(NF_Virtual);
					break;
				}
			}
			pAncestor = pAncestor->SuperClass();
		}
	}
}

void StatementResolveAccessor::Access(SnClassDecl &sn)
{
	assert(m_pVisitor);
	ResolveClassBases(sn);
	PropagateVirtualOverrides(sn);
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_ClassField)
		{
			//0.9.0 semantic restriction: class field names may not
			//reuse visible type names.
			RejectTypeNameVariable(field.Name(), sn, sn,
				field.Location());
			field.Accept(*m_pVisitor);
		}
	}
	//Resolve method bodies. Phase 9d-2: track the enclosing class so
	//super(...) statements can validate against it.
	m_pCurrClass = &sn;
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_Function)
			field.Accept(*m_pVisitor);
	}
	m_pCurrClass = nullptr;
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnClassField &sn)
{
	assert(m_pVisitor);
	if (sn.Type())
		sn.Type()->Accept(*m_pVisitor);
}

//Phase 9d: walk the SuperClass chain of t (if any). Returns true if any
//ancestor class is named "Exception" (case-sensitive). Also returns true
//when t itself is "Exception".
bool StatementResolveAccessor::IsExceptionSubclass(SnField *t)
{
	if (!t)
		return false;
	//Catch types are always class-typed (resolved by SnClassDecl).
	auto pClass = dynamic_cast<SnClassDecl*>(t);
	if (!pClass)
		return false;
	SnClassDecl *cur = pClass;
	while (cur) {
		if (cur->Name() == "Exception")
			return true;
		cur = cur->SuperClass();
	}
	return false;
}

//Declaration contract of a single interface member (0.9.10): public by
//default (FA_Default already resolved to FA_Public by OnAddedToParent,
//so a non-public member carries an explicit private/protected modifier),
//no const/native/virtual, and of the static/instance pair only the legal
//shapes pass — a static member carries a body, an instance member does
//not (D1). Rejecting at the declaration replaces the old failure mode
//where a silently-private member surfaced only at call sites as a
//misleading "does not exist or is not accessible" error.
static void CheckInterfaceMemberContract(BuildEnvironment &env,
	SnField &field)
{
	if (field.AccessType() != FA_Public)
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" must be public.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Const))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be const: interfaces"
			" declare no data to freeze.",
			field.Name().c_str());
	//0.9.10 (D1): a static member is legal as an in-place shared
	//implementation; only the bodyless static and the bodied
	//instance forms break the contract now.
	auto *fn = field.Kind() == NK_Function
		? static_cast<SnFunction *>(&field) : nullptr;
	if (fn && fn->ContainFlags(NF_Static) && !fn->Body())
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" is static but has no body: "
			"static interface methods are implemented in place.",
			field.Name().c_str());
	if (fn && !fn->ContainFlags(NF_Static) && fn->Body())
		env.Log(CLL_Error, field.Location(),
			"The interface instance method \"%s\" cannot have a body: "
			"use a static method for shared implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Native))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be native: interfaces"
			" declare signatures without implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Virtual))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be virtual: interface"
			" methods dispatch by name already.",
			field.Name().c_str());
}

void StatementResolveAccessor::Access(SnInterfaceDecl &sn)
{
	//Enforce the member contract at the single point every interface
	//declaration passes through.
	for (auto &field : sn.Members())
		CheckInterfaceMemberContract(m_Env, field);
	//Interface member signatures resolve like class methods; only the
	//static members carry bodies (0.9.10), walked like any function
	//body. No super-class chain to walk.
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_Function)
			field.Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
