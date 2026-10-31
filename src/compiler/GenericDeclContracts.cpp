/*---
    GenericDeclContracts.cpp — declaration-level contract checks for
    generic class declarations (CheckGenericDeclContract). Split from
    ModuleBuilderMethodContracts.cpp (2026-10-27 size-guard split);
    rewritten for the v0.12.3 dictionary model: members mix freely
    (method bodies, fields, native members in one template row).
---*/
#include "ModuleBuilderChecks.h"
#include "BuildEnvironment.h"
#include "builder/ModuleRegistry.h"
#include "SyntaxTree.h"

namespace nlang
{

namespace
{
//A member named like one of the declaration's type parameters would
//shadow the parameter inside the declaration's scope, where the
//parameter name IS a type (spec §8: collision is a declaration-level
//error, checked for every member kind).
void CheckMemberNotTypeParamName(const SnClassDecl &cls,
	const SnField &member, BuildEnvironment &env)
{
	const auto &names = cls.TypeParamNames();
	const std::string &memberName = member.Name();
	for (const auto &paramName : names)
		if (memberName == paramName)
		{
			env.Log(CLL_Error, member.Location(),
				"The class \"%s\" is a generic declaration: the member "
				"\"%s\" collides with the type parameter \"%s\" (the "
				"parameter name is a type in this declaration's scope).",
				cls.Name().c_str(), memberName.c_str(),
				paramName.c_str());
			return;
		}
}

//Null unless the declared type is a bare type-parameter reference (T,
//not T[] or List<T>); returns the parameter name. Type-position
//resolution binds such a reference to the template's interned
//placeholder class, whose name IS the parameter name — concrete types
//bind to their own declarations and composite T forms never bind to
//the placeholder itself, so binding identity is the exact predicate
//(robust against the identifier-to-name-expression rewrite resolution
//performs on the way).
const std::string *BareTypeParamName(const SnFieldExpr *pType,
	BuildEnvironment &env)
{
	if (!pType)
		return nullptr;
	const SnField *pBound = pType->Field();
	if (!pBound && pType->Kind() == NK_NameExpr)
		if (const SnFieldExpr *pInner =
			static_cast<const SnNameExpr *>(pType)->Expr())
			pBound = pInner->Field();
	if (!pBound
		|| !env.Registry().DeclaredGenerics().IsPlaceholder(pBound))
		return nullptr;
	return &pBound->Name();
}
} //namespace

//v0.12.3 dictionary model: a T[] TYPE position is a legal handle
//position (the token interns over the placeholder, one slot whatever
//the instantiation binds), but array CREATION over T has no element
//kind until the instantiation — `T a[3]` is the sized-declarator form
//of new T[3], the same rejection family as new T[n] in bodies (spec
//D2). The sized-declarator intern (StatementResolveAccessor's
//local-declaration path) is the one remaining caller.
bool RejectTypeParamArrayElement(SnField *pElemField,
	BuildEnvironment &env, const ISourceLocation *pLoc)
{
	if (!pElemField
			|| !env.Registry().DeclaredGenerics().IsPlaceholder(pElemField))
		return false;
	env.Log(CLL_Error, pLoc,
		"Cannot create an array of the type parameter \"%s\": the "
		"element kind is not known until the instantiation.",
		pElemField->Name().c_str());
	return true;
}

//v0.12.3 dictionary model: a generic declaration is one template row —
//members with bodies compile once against placeholder types, native
//members keep their lazy host binding, and the two mix freely. The
//remaining declaration-level rules: a member neither native nor with
//a body has no implementation; a bare type-parameter type has no
//concrete kind, so its field initializers and parameter defaults have
//no meaningful value.
void CheckGenericDeclContract(SnClassDecl &cls, BuildEnvironment &env)
{
	if (!cls.IsGenericDeclaration())
		return;
	for (auto &member : cls.Members())
	{
		CheckMemberNotTypeParamName(cls, member, env);
		if (member.Kind() == NK_ClassField)
		{
			auto &field = static_cast<SnDataField &>(member);
			const std::string *pParamName =
				BareTypeParamName(field.Type(), env);
			if (field.Value() && pParamName)
				env.Log(CLL_Error, member.Location(),
					"The field \"%s\" of generic class \"%s\" cannot "
					"have an initializer: the type parameter \"%s\" "
					"has no concrete kind, so no initial value exists "
					"(constructors may initialize it).",
					member.Name().c_str(), cls.Name().c_str(),
					pParamName->c_str());
			continue;
		}
		if (member.Kind() != NK_Function)
			continue;
		auto &func = static_cast<SnFunction &>(member);
		//HasImplementation without the IsImported arm: at source-check
		//time the declaration is never an import stub, so the predicate
		//reduces to native-or-body — the same shape the ctor candidate
		//collector keys off.
		if (!func.HasImplementation())
			env.Log(CLL_Error, member.Location(),
				"The method \"%s\" of generic class \"%s\" must be "
				"native or have a method body.",
				member.Name().c_str(), cls.Name().c_str());
		for (auto &param : func.Params())
		{
			const std::string *pParamName =
				BareTypeParamName(param.Type(), env);
			if (param.Value() && pParamName)
				env.Log(CLL_Error, param.Location(),
					"The parameter \"%s\" of the method \"%s\" cannot "
					"have a default value: the type parameter \"%s\" "
					"has no concrete kind, so no default value exists.",
					param.Name().c_str(), member.Name().c_str(),
					pParamName->c_str());
		}
	}
}

} //namespace nlang
