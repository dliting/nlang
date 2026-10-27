/*---
    GenericDeclContracts.cpp — 泛型类声明的"仅签名"契约检查
    （CheckGenericDeclNativeOnly）。从 ModuleBuilderMethodContracts.cpp
    拆出（2026-10-27 尺寸守卫触发的拆分，零行为变化）。
---*/
#include "ModuleBuilderChecks.h"
#include "BuildEnvironment.h"
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
} //namespace

//Generic class declarations carry signature surfaces only: the
//declaration grammar has no generic method compilation, so every
//method member of a declared generic must be native, without
//parameter default values (instantiations copy signatures, and the
//default-expression machinery has no generic shape). Field members
//carry no initializers for the same reason.
void CheckGenericDeclNativeOnly(SnClassDecl &cls,
	BuildEnvironment &env)
{
	if (!cls.IsGenericDeclaration())
		return;
	for (auto &member : cls.Members())
	{
		CheckMemberNotTypeParamName(cls, member, env);
		if (member.Kind() != NK_Function)
		{
			if (member.Kind() == NK_ClassField)
			{
				auto &field = static_cast<SnDataField &>(member);
				if (field.Value())
					env.Log(CLL_Error, member.Location(),
						"The class \"%s\" is a generic declaration: the "
						"field \"%s\" cannot have an initializer "
						"(generic declarations carry signatures only).",
						cls.Name().c_str(), member.Name().c_str());
			}
			continue;
		}
		auto &func = static_cast<SnFunction &>(member);
		if (!func.ContainFlags(NF_Native))
			env.Log(CLL_Error, member.Location(),
				"The class \"%s\" is a generic declaration: the method "
				"\"%s\" must be native (generic declarations carry "
				"signatures only).",
				cls.Name().c_str(), member.Name().c_str());
		for (auto &param : func.Params())
			if (param.Value())
				env.Log(CLL_Error, param.Location(),
					"The class \"%s\" is a generic declaration: the "
					"parameter \"%s\" of \"%s\" cannot have a default "
					"value (generic declarations carry signatures "
					"only).",
					cls.Name().c_str(), param.Name().c_str(),
					member.Name().c_str());
	}
}

} //namespace nlang
