/*---
    StatementResolverInterface.cpp — the declaration contracts of
    interface members: the constant face, the constant/method name
    ambiguity, member visibility and flag surface, and interface member
    signature resolution. Extracted from StatementResolverTypes.cpp
    (2026-11-13, 0.13.1c, source-size guard).
---*/
#include "StatementResolver.h"
#include "FieldInitializerChecks.hpp"
#include "BuildEnvironment.h"
#include "ModuleBuilderChecks.h"
#include <nlang/compiler/SnExpressions.h>

namespace nlang
{

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

//0.13.1c: a T-typed formal's default value has no concrete kind to
//construct from (the class contract's gate, mirrored). The member type
//expressions resolved in the earlier ResolveDataTypes pass, so the
//placeholder binding is readable here. Non-generic interfaces cannot
//trip it (no template scope, so no placeholder binding).
void RejectTypeParamParamDefaults(BuildEnvironment &env, SnFunction &fn)
{
	for (auto &param : fn.Params())
	{
		const std::string *pParamName =
			BareTypeParamName(param.Type(), env);
		if (param.Value() && pParamName)
			env.Log(CLL_Error, param.Location(),
				"default value for parameter \"%s\": the type "
				"parameter \"%s\" has no concrete kind, so no "
				"default value exists.",
				param.Name().c_str(), pParamName->c_str());
	}
}

//NK_ClassField arm of the member contract: the 0.13.1c T-typed-constant
//gate (no value domain until the instantiation — nothing exists to fold
//or store) plus the shared constant contract. Non-generic interfaces
//cannot trip the gate (no template scope, so no placeholder binding).
void CheckInterfaceConstantMember(BuildEnvironment &env,
	SnClassField &classField)
{
	if (const std::string *pParamName =
		BareTypeParamName(classField.Type(), env))
		env.Log(CLL_Error, classField.Location(),
			"The interface constant \"%s\" cannot have the type "
			"parameter \"%s\" as its type.",
			classField.Name().c_str(), pParamName->c_str());
	CheckInterfaceConstantContract(env, classField);
}

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
		CheckInterfaceConstantMember(env,
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
	if (fn)
		RejectTypeParamParamDefaults(env, *fn);
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
