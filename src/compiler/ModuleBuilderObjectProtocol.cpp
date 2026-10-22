/*---
ModuleBuilderObjectProtocol.cpp - Object-protocol signature table of
the method-contract checks (spec D4): the canonical equals/getHashCode/
toString signatures, per-method matching and the mismatch diagnostic.
Split from ModuleBuilderChecks.cpp (2026-10-17 maintainability
refactor, zero behavior change).
---*/

#include "ModuleBuilder.h"
#include "ModuleBuilderChecks.h"
#include "SyntaxTree.h"
#include "builder/ExprResolver.h"
#include <string>
#include <vector>

namespace nlang
{

namespace {

//Canonical Object-protocol signatures (spec D4): the Object builtin is
//an empty stub with no AST and no chain presence (a user class without
//a declared base has SuperClass()==nullptr, not Object), so the
//protocol is a hardcoded table. Identity compares by POINTER against
//the builtin singletons — the resolver binds "Object"/int/string type
//names to those same nodes, and name rendering would wrongly unify
//equals(SomeUserClass) with equals(Object) (both render "class ...").
struct ProtocolEntry
{
	const char *methodName;
	size_t paramCount;        //0 or 1
	NodeKind returnKind;      //NK_Int32 or NK_String
	const char *rendered;     //for diagnostics
};

const ProtocolEntry* FindProtocolEntry(const std::string &name)
{
	//rendered must agree with the row's paramCount/returnKind fields
	//(two sources of truth for the same signature).
	static const ProtocolEntry kTable[] = {
		{ "equals", 1, NK_Int32, "equals(Object): Int32" },
		{ "getHashCode", 0, NK_Int32, "getHashCode(): Int32" },
		{ "toString", 0, NK_String, "toString(): String" },
	};
	for (auto &e : kTable)
		if (name == e.methodName)
			return &e;
	return nullptr;
}

//Parameter-list half of the protocol identity. 0.9.10 D2.4: the
//(name, parameter list) pair is the dispatch-slot identity, shared by
//the duplicate-declaration and static-collision rules.
bool ParamListMatchesProtocol(const SnFunction &f, const ProtocolEntry &e,
	SnField *pObjectClass)
{
	if (f.Params().size() != e.paramCount)
		return false;
	if (e.paramCount == 1)
	{
		for (auto &param : f.Params())
		{
			auto *pType = param.Type() ? param.Type()->Field() : nullptr;
			if (pType != pObjectClass || param.IsArrayType()
				|| param.ContainFlags(NF_Out))
				return false;
		}
	}
	return true;
}

bool MatchesProtocolEntry(const SnFunction &f, const ProtocolEntry &e,
	SnField *pObjectClass, SnField *pInt, SnField *pString)
{
	//A static method cannot claim the protocol's dispatch slot either.
	if (f.ContainFlags(NF_Static))
		return false;
	if (!ParamListMatchesProtocol(f, e, pObjectClass))
		return false;
	SnField *pRetExpected =
		(e.returnKind == NK_String) ? pString : pInt;
	auto *pRet = f.ReturnType() ? f.ReturnType()->Field() : nullptr;
	return pRet == pRetExpected;
}

} //namespace

//Object protocol (spec D4): these three names carry the implicit
//Object contract in every user class. Check against every own
//same-name method; a mismatch rejects the whole name (subclasses
//skip it — same recovery as the bijection in ModuleBuilderMethodContracts.cpp).
bool CheckObjectProtocol(SnClassDecl &cls, const std::string &name,
	const std::vector<SnFunction*> &funcs, BuildEnvironment &env)
{
	const ProtocolEntry *pProtocol = FindProtocolEntry(name);
	if (!pProtocol)
		return false;
	//Location is only the fallback error anchor for the lookup
	//itself; the class declaration site is the natural context.
	auto *pObjectClass =
		GetBuiltinClassDecl("Object", cls.Location());
	auto *pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
	auto *pString = SnBuiltinDataType::InstanceOf(NK_String);
	bool protocolRejected = false;
	for (auto *pOwn : funcs)
	{
		//0.9.10 D2.4: a static with a different parameter list lives in
		//the static namespace and never competes with the inherited
		//Object method. A same-parameter-list static still claims the
		//dispatch slot — the 0.9.7 collision rule.
		if (pOwn->ContainFlags(NF_Static))
		{
			if (!ParamListMatchesProtocol(*pOwn, *pProtocol, pObjectClass))
				continue;
		}
		else if (MatchesProtocolEntry(*pOwn, *pProtocol,
				pObjectClass, pInt, pString))
			continue;
		env.Log(CLL_Error, pOwn->Location(),
			"The method \"%s\" claims the virtual dispatch "
			"slot of \"Object.%s\" with a different "
			"signature \"%s\". Align the signature to "
			"override, or rename the method.",
			pOwn->Name().c_str(), pProtocol->rendered,
			FormatSignature(name, *pOwn).c_str());
		protocolRejected = true;
	}
	return protocolRejected;
}

} //namespace nlang
