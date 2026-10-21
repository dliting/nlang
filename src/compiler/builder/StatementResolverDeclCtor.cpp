/*---
    StatementResolverDeclCtor.cpp — 构造器相关声明处理：构造器声明契约与
    形状谓词（0.9.11 D1），以及直接初始化声明的合成降级：
    `T name(args);` 分解为与 `T name = new T(args);` 相同的赋值语句树。
    自 StatementResolverDecls.cpp 抽出（0.9.0，源文件 500 行守卫）。
---*/
#include "StatementResolver.h"
#include <list>

namespace nlang
{

//0.9.11 D1: relaxed declaration-shape predicate for the constructor
//contract — a member function whose name equals the enclosing type's
//name, regardless of flags (the contract must also see the static/
//virtual/const spellings the no-return productions admit). 0.9.11 D2:
//struct constructors share the shape rule; the struct member productions
//have no return-type slot, so the contract's return checks are inert
//there and only the flag contract applies.
bool StatementResolveAccessor::IsCtorNameShape(SnFunction& sn)
{
	auto* pParent = sn.Parent();
	if (!pParent)
		return false;
	if (pParent->Kind() == NK_ClassDecl)
		return sn.Name() == static_cast<SnClassDecl*>(pParent)->Name();
	if (pParent->Kind() == NK_StructDecl)
		return sn.Name() == static_cast<SnStructDecl*>(pParent)->Name();
	return false;
}

//P3 review: true when sn is a constructor — an instance member function
//of a class whose name equals the class name (the same declaration-shape
//rule FindCtorParamRange applies to class members). 0.9.10 D2.4: a
//same-name static lives in the static namespace, never in the ctor slot.
bool StatementResolveAccessor::IsConstructorFunction(SnFunction& sn)
{
	return IsCtorNameShape(sn) && !sn.ContainFlags(NF_Static);
}

//0.9.11 D1: constructor declaration contract. A class member whose
//name equals the class name is a constructor: it must not declare a
//return type (typed or 'void') and must not be static/virtual/const
//(a same-name static WITH a return type is a plain static method and
//stays legal). A no-return member with a different name is a plain
//function missing its return type. Imported stubs are synthesized
//with a null return type and skip the contract.
//Check order is diagnostic priority, not a bit-layout constraint:
//NF_ExplicitVoid is a single compile-only bit (bit 24) disjoint from the
//runtime flags, but a `virtual void C()` spelling carries both, and the
//precise "must not declare 'void'" text should win over the generic flag
//text — so the return-type checks run first. The flag check uses two
//single-bit ContainFlags calls: ContainFlags is all-bits ((m_Flags &
//bits) == bits, Flagable.h), so a combined NF_Virtual|NF_Const mask
//would demand both flags at once.
void StatementResolveAccessor::CheckCtorDeclarationContract(SnFunction &sn)
{
	if (sn.IsImported())
		return;
	if (IsCtorNameShape(sn)) {
		if (!sn.ContainFlags(NF_Static)) {
			if (sn.HasReturn())
				m_Env.Log(CLL_Error, sn.Location(),
					"constructor \"%s\" must not declare a return type",
					sn.Name().c_str());
			else if (sn.ContainFlags(NF_ExplicitVoid))
				m_Env.Log(CLL_Error, sn.Location(),
					"constructor \"%s\" must not declare 'void'; "
					"remove the return type",
					sn.Name().c_str());
			else if (sn.ContainFlags(NF_Virtual) || sn.ContainFlags(NF_Const))
				m_Env.Log(CLL_Error, sn.Location(),
					"constructor \"%s\" cannot be declared static, "
					"virtual, or const",
					sn.Name().c_str());
		} else if (!sn.HasReturn() && !sn.ContainFlags(NF_ExplicitVoid)) {
			m_Env.Log(CLL_Error, sn.Location(),
				"constructor \"%s\" cannot be declared static, "
				"virtual, or const",
				sn.Name().c_str());
		}
	} else if (sn.Parent() && sn.Parent()->Kind() == NK_ClassDecl
		&& !sn.HasReturn() && !sn.ContainFlags(NF_ExplicitVoid)) {
		m_Env.Log(CLL_Error, sn.Location(),
			"function \"%s\" must declare a return type",
			sn.Name().c_str());
	}
}

//0.9.11 D2: struct member contract. A struct declares fields and
//constructors only: a function member whose name differs from the
//struct's is not a constructor and is rejected outright. The flag
//contract (no static/virtual/const) is shared with class — IsCtorNameShape
//covers struct members, so CheckCtorDeclarationContract emits the single
//flag diagnostic when the member resolves. The no-return shape is
//structural here: the struct member productions have no return-type slot.
//0.9.12 D7 (D-new-2): struct members are public-only — the bare default
//was already promoted to FA_Public in OnAddedToParent, so a spelled-out
//private/protected is rejected. Imported stubs are producer-validated and
//skip both checks.
void StatementResolveAccessor::CheckStructCtorContract(
	SnStructDecl &owner, SnFunction &sn)
{
	if (sn.IsImported())
		return;
	if (sn.Name() != owner.Name()) {
		m_Env.Log(CLL_Error, sn.Location(),
			"struct \"%s\" may only declare fields and constructors",
			owner.Name().c_str());
	}
	if (sn.AccessType() != FA_Public) {
		m_Env.Log(CLL_Error, sn.Location(),
			"The struct member \"%s\" must be public.",
			sn.Name().c_str());
	}
}

SnAssignStmt *StatementResolveAccessor::BuildCtorInitAssign(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl)
{
	//Generic type heads (SnGenericTypeExpr) have no class declaration a
	//fresh SnNewExpr could bind — reject by name instead of letting
	//Access(SnNewExpr&) produce its generic not-a-class text.
	if (sn.Type()->Kind() == NK_GenericTypeExpr)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"generic class \"%s\" does not support direct-init; use [] or "
			"{} with a typed declaration",
			sn.Type()->ToString().c_str());
		return nullptr;
	}
	//The fresh class-name field binds the ALREADY-RESOLVED declaration
	//type directly (ResolveFieldExprAs): value-position name lookup would
	//find shadowing locals first, and built-in classes have no namespace
	//entry to find — the same bypass TryResolveBuiltinClassName uses.
	auto *pClassName = new SnIdentifierExpr(
		new std::string(sn.Type()->ToString()), *sn.Location());
	m_ExprResolver.ResolveFieldExprAs(*pClassName, sn.Type()->Field());
	//Detach the args from this statement's children list (dual storage)
	//into the SnNewExpr; the caller drops the emptied payload container.
	//The args list takes the same inner-collection form the grammar's
	//ConcreteParamList hands to SnNewExpr (an owning UniquePtrList is
	//built from it inside the call).
	auto *pArgs = new std::list<SnExpression *>();
	for (auto *pArg : *decl.pCtorArgs)
		pArgs->push_back(sn.DetachChild(pArg));
	auto *pNew = new SnNewExpr(pClassName, pArgs, *sn.Location());
	auto *pLeft = new SnIdentifierExpr(
		new std::string(decl.name), *sn.Location());
	return new SnAssignStmt(pLeft, pNew, *sn.Location());
}

//Consume a `Type name(args)` declarator: build the decomposition assign,
//insert it at iInsert and resolve it. Accept(SnAssignStmt&) drives
//Access(SnNewExpr&) (class check, arity, argument resolution) and the
//LHS/cast tail. The emptied ctor-arg container is deleted either way so
//the consumed slots never reach codegen.
void StatementResolveAccessor::SynthesizeCtorInitDeclarator(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl,
	SnParagraph &paragraph, NodeIterator iInsert)
{
	auto *pAssign = BuildCtorInitAssign(sn, decl);
	delete decl.pCtorArgs;
	decl.pCtorArgs = nullptr;
	if (pAssign)
	{
		paragraph.InsertChild(iInsert, pAssign);
		pAssign->Accept(*m_pVisitor);
	}
}

} //namespace nlang
