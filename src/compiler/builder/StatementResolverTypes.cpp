/*---
    StatementResolverTypes.cpp — 类型声明解析：struct/class/interface 成员遍历与 Exception 子类判定。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include "FieldInitializerChecks.hpp"
#include "ConstAggregateSurface.hpp"
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

namespace
{

//v0.12.4 D1 (user ruling 2026-10-31), both directions: const classes
//are extended by const classes only. A sealed base with a mutable
//subclass would need per-subclass seal bookkeeping; a const subclass
//of a mutable base is sealed in name only — the base's own
//constructors and methods keep writing the inherited fields. Object
//is the exempt hierarchy root: it declares no fields, and every const
//class reaches it. Fires on the first binding only (PreResolveClassBases
//re-runs with SuperClass still unbound; the != guard holds either way).
void CheckConstInheritance(BuildEnvironment &env, SnClassDecl &sn,
	SnClassDecl &super)
{
	if (super.IsConstAggregate()
		&& !sn.IsDeclaredConstAggregate()
		&& sn.SuperClass() != &super)
		env.Log(CLL_Error, sn.SuperName()->Location(),
			"The class \"%s\" extends the const class \"%s\": "
			"subclasses of a const class must be const too.",
			sn.Name().c_str(), super.Name().c_str());
	else if (sn.IsDeclaredConstAggregate()
		&& !super.IsConstAggregate()
		&& sn.SuperClass() != &super
		&& &super != FindPreludeClassDecl(env.Registry(), "Object"))
		env.Log(CLL_Error, sn.SuperName()->Location(),
			"The class \"%s\" extends the mutable class \"%s\": "
			"a const class may extend const classes only.",
			sn.Name().c_str(), super.Name().c_str());
}

} //namespace

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
	NLANG_ASSERT(m_pVisitor);
	for (auto &field : sn.Fields())
	{
		//0.9.12 D7 (D-new-2): a struct is a public data record — every
		//member is public. The bare default was already promoted to
		//FA_Public in OnAddedToParent, so anything left non-public was
		//spelled out and is a declaration error.
		if (field.AccessType() != FA_Public)
			m_Env.Log(CLL_Error, field.Location(),
				"The struct member \"%s\" must be public.",
				field.Name().c_str());
		//0.9.0 semantic restriction: struct field names may not reuse
		//visible type names.
		RejectTypeNameVariable(field.Name(), sn, sn, field.Location());
		//v0.12.6 D8 defense-in-depth: StructField takes no grammar flags,
		//so a flagged struct field is unparseable today — the check keeps
		//a future flag arm from silently admitting statics.
		if (field.ContainFlags(NF_Static))
			m_Env.Log(CLL_Error, field.Location(),
				"The struct type \"%s\" cannot declare static fields.",
				sn.Name().c_str());
		field.Type()->Accept(*m_pVisitor);
		//0.9.11 D4: the initializer's constant surface, checked once the
		//type expression resolved (the same shared surface as class
		//fields; the struct default itself is applied at allocation,
		//not by constructor injection).
		CheckFieldInitializer(m_Env, field);
		//v0.12.4 D6: the sealed surface of a const struct.
		if (sn.IsConstAggregate())
			CheckConstAggregateFieldSurface(m_Env, sn, "struct", field);
	}
	//0.9.11 D2: struct constructors. The member contract rejects plain
	//function members; the flag contract is shared with class through
	//IsCtorNameShape and fires from CheckCtorDeclarationContract when the
	//member resolves. Body scoping (this/super) reads the AST parent
	//chain, not a tracker member: m_pCurrClass stays null here, which is
	//exactly what rejects super(...) inside a struct constructor.
	for (auto &ctor : sn.Ctors())
	{
		CheckStructCtorContract(sn, ctor);
		ctor.Accept(*m_pVisitor);
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
			if (pSuperField && pSuperField->Kind() == NK_ClassDecl
				&& RejectPreludeFaceUse(m_Env, pSuperField,
					sn.SuperName()->Location(), "extended"))
			{
				//v0.12.2: face bases are rejected — the class stays
				//baseless (faces own no instances or dispatch shapes
				//to inherit).
			}
			else if (pSuperField && pSuperField->Kind() == NK_ClassDecl)
			{
				auto *pSuperDecl = static_cast<SnClassDecl*>(pSuperField);
				//v0.12.4 D1 (user ruling 2026-10-31): const classes are
				//extended by const classes only — a mixed hierarchy
				//(sealed base, mutable subclass) would need per-subclass
				//seal bookkeeping later. PreResolveClassBases re-runs this
				//binding in order, so the diagnostic fires on the first
				//binding only (SuperClass is still unbound then).
				CheckConstInheritance(m_Env, sn, *pSuperDecl);
				sn.SuperClass(pSuperDecl);
			}
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

//v0.12.6 D1: class static-field declaration contract. The legal flag
//sets are NF_Static and NF_Static|NF_Const (NodeFlagsNonEmpty yields
//only const/static/virtual/native); a static constant needs its
//initializer (the declaration IS the initializer — no zero-default
//form); a declared generic class carries no static fields (D8: one
//shared implementation serves every instantiation, so per-class state
//has no home).
static void CheckClassFieldStaticContract(BuildEnvironment &env,
	SnClassDecl &owner, SnField &field)
{
	const bool bStatic = field.ContainFlags(NF_Static);
	const bool bConst = field.ContainFlags(NF_Const);
	if ((bStatic || bConst)
		&& (field.ContainFlags(NF_Virtual) || field.ContainFlags(NF_Native)
			|| (bConst && !bStatic)))
	{
		env.Log(CLL_Error, field.Location(),
			"The class field \"%s\" must be static or static const.",
			field.Name().c_str());
		return;
	}
	if (bStatic && bConst && field.Kind() == NK_ClassField
		&& !static_cast<SnClassField &>(field).Initializer())
	{
		env.Log(CLL_Error, field.Location(),
			"The static constant \"%s\" must have an initializer.",
			field.Name().c_str());
		return;
	}
	if (bStatic && owner.IsGenericDeclaration())
		env.Log(CLL_Error, field.Location(),
			"The generic class \"%s\" cannot declare static fields.",
			owner.Name().c_str());
}

void StatementResolveAccessor::Access(SnClassDecl &sn)
{
	NLANG_ASSERT(m_pVisitor);
	//Builtin-decls source of truth: the implicit Object chain must be
	//live before PropagateVirtualOverrides walks the ancestors.
	EnsureImplicitObjectSuperclass(sn, m_Env.Registry());
	ResolveClassBases(sn);
	PropagateVirtualOverrides(sn);
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_ClassField)
		{
			//v0.12.6 D1: the static-field contract runs where the owning
			//class is in hand (the generic ban needs it). Imported stubs
			//passed the contract at the producer. Intersect (any of the
			//two bits), not Contain: ContainFlags is the all-bits test,
			//and `static virtual int x;` carries no NF_Const to satisfy it.
			if (!sn.IsImported()
				&& field.IntersectFlags(NF_Static | NF_Const))
				CheckClassFieldStaticContract(m_Env, sn, field);
			//0.9.0 semantic restriction: class field names may not
			//reuse visible type names.
			RejectTypeNameVariable(field.Name(), sn, sn,
				field.Location());
			field.Accept(*m_pVisitor);
			//v0.12.4 D6: the sealed surface of a const class. Runs after
			//the field's type resolved (Access(SnClassField&) above).
			if (sn.IsConstAggregate())
				CheckConstAggregateFieldSurface(m_Env, sn, "class", field);
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
	NLANG_ASSERT(m_pVisitor);
	if (sn.Type())
		sn.Type()->Accept(*m_pVisitor);
	//v0.12.6 D1: a static field's initializer is lazy runtime code (D2),
	//not a compile-time instance default — the constant-surface check
	//does not apply. Foldable `static const` folding arrives with the
	//runtime-const tasks.
	if (sn.ContainFlags(NF_Static))
		return;
	//0.9.11 D4: the field's type expression resolved in the earlier
	//ResolveDataTypes pass, so the initializer's constant surface is
	//checkable here. Interface constants never reach this arm (the
	//interface decl arm walks only functions); their contract checks
	//the same surface through CheckInterfaceConstantContract.
	CheckFieldInitializer(m_Env, sn);
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

namespace
{

//0.9.10 D3.3 + v0.12.6 D11/D14/D15: an interface constant takes any
//declared type a class static field takes. Scalars and strings fold at
//compile time (the full declaration-time fold/domain/fit surface); every
//other type — structs since D14, classes, arrays, containers and
//enums since D15 — is a runtime constant: the interface's synthesized
//$staticInit stores it lazily, and the ordinary assignment machinery
//type-checks the expression. 0.9.10's "object constants need shared
//runtime storage" objection died with D11's runtime slots.
void CheckInterfaceConstantInitializer(BuildEnvironment &env,
	SnClassField &field)
{
	SnField *pDeclared = field.EvalDataType();
	if (!pDeclared)
		return;	//the type expression already failed to resolve
	if (!field.Initializer())
	{
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" must have an initializer.",
			field.Name().c_str());
		return;
	}
	//Fold surface is a scalar/string matter (StaticConstFoldsAtCompileTime
	//mirrors this arm); all other declared types ride the runtime channel
	//whatever their initializer looks like.
	if (!IsConstantValueType(pDeclared)
		|| ClassifyConstantInitializer(field.Initializer())
		== ConstantFoldKind::NotFoldable)
		return;	//runtime constant — checked via the synthesized store
	CheckConstantValueSurface(env, field, field.Initializer(),
		*pDeclared, "constant");
}

//0.9.10 D3.7: a constant and a method of the same name in one interface
//make the value form and the call form ambiguous. The generic duplicate
//checker also reports the name collision; this diagnostic names the
//constant/method ambiguity itself.
void CheckInterfaceConstantNameClash(BuildEnvironment &env,
	SnInterfaceDecl &interfaceDecl, SnField &constant)
{
	for (auto &member : interfaceDecl.Members())
	{
		if (&member == &constant || member.Kind() != NK_Function
			|| member.Name() != constant.Name())
			continue;
		env.Log(CLL_Error, constant.Location(),
			"The interface constant \"%s\" is ambiguous with the method "
			"of the same name in interface \"%s\".",
			constant.Name().c_str(), interfaceDecl.Name().c_str());
		return;
	}
}

//0.9.10 D3.3: interface member variables exist in exactly one form,
//`static const Type NAME = <folded literal>;`.
void CheckInterfaceConstantContract(BuildEnvironment &env,
	SnClassField &field)
{
	//NodeFlagsNonEmpty only yields these four flags; the exact legal set
	//is static+const with neither virtual nor native. The static/const
	//requirement is one diagnostic; the method-only modifiers are named
	//individually, mirroring the method contract below.
	if (!field.ContainFlags(NF_Static) || !field.ContainFlags(NF_Const))
	{
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" must be static const: "
			"interfaces declare constants only.",
			field.Name().c_str());
		return;
	}
	if (field.ContainFlags(NF_Native))
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" cannot be native: constants "
			"are data, not implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Virtual))
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" cannot be virtual: constants "
			"are values, not methods.",
			field.Name().c_str());
	CheckInterfaceConstantInitializer(env, field);
}

} //namespace

//Declaration contract of a single interface member (0.9.10): public by
//default (FA_Default already resolved to FA_Public by OnAddedToParent,
//so a non-public member carries an explicit private/protected modifier).
//Methods carry no const/native/virtual flag, and of the static/instance
//pair only the legal shapes pass — a static member carries a body, an
//instance member does not (D1). Fields exist only as `static const`
//constants (D3). Rejecting at the declaration replaces the old failure
//mode where a silently-private member surfaced only at call sites as a
//misleading "does not exist or is not accessible" error.
static void CheckInterfaceMemberContract(BuildEnvironment &env,
	SnField &field)
{
	if (field.AccessType() != FA_Public)
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" must be public.",
			field.Name().c_str());
	if (field.Kind() == NK_ClassField)
	{
		CheckInterfaceConstantContract(env,
			static_cast<SnClassField &>(field));
		return;
	}
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
	//0.9.10 D5: an imported stub carries wire-rebuilt static members
	//that are bodyless by construction — the member contract is
	//enforced at the producer's declaration. Only the signatures
	//resolve here (call sites bind against them).
	if (!sn.ContainFlags(NF_Imported))
	{
		//Enforce the member contract at the single point every own
		//interface declaration passes through.
		for (auto &field : sn.Members())
		{
			CheckInterfaceMemberContract(m_Env, field);
			if (IsInterfaceConstantField(field))
				CheckInterfaceConstantNameClash(m_Env, sn, field);
		}
	}
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
