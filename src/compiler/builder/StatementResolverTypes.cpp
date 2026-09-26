/*---
    StatementResolverTypes.cpp — 类型声明解析：struct/class/interface 成员遍历与 Exception 子类判定。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"

namespace nlang
{

void StatementResolveAccessor::Access(SnStructDecl &sn)
{
	assert(m_pVisitor);
	for (auto &field : sn.Members())
	{
		field.Type()->Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnStructField &sn)
{
}

void StatementResolveAccessor::Access(SnClassDecl &sn)
{
	assert(m_pVisitor);
	//Resolve super class reference using ExprResolver (not the
	//StatementResolver visitor, which lacks Access(SnNameExpr&)).
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
	//Resolve "implements I1, I2" names into SnInterfaceDecl* pointers.
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
	//A method that overrides a parent virtual method
	//is also virtual (implicit virtual propagation).
	//Check both name and parameter count to avoid false matches.
	auto *pSuper = sn.SuperClass();
	if (pSuper)
	{
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
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_ClassField)
			field.Accept(*m_pVisitor);
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

void StatementResolveAccessor::Access(SnInterfaceDecl &sn)
{
	//Interface method signatures are resolved like class methods but
	//they have no bodies (the resolver tolerates an empty body when
	//NF_Abstract is set). No super-class chain to walk.
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_Function)
			field.Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
