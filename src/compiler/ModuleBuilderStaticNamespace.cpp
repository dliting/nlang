/*---
ModuleBuilderStaticNamespace.cpp - static-member name rules of the
method-contract check family (0.9.10 D2.4): statics live in a separate
namespace from instance dispatch, but a same-parameter-list static
still claims the dispatch slot of an inherited virtual method or of an
Object protocol method. Split from ModuleBuilderMethodContracts.cpp
(2026-10-18 source-size split); the static/instance namespace rule is
the 0.9.10 behavior change itself.
---*/

#include "ModuleBuilder.h"
#include "ModuleBuilderChecks.h"
#include "SyntaxTree.h"
#include <set>
#include <string>
#include <vector>

namespace nlang
{

namespace {

//Emit the two-line diagnostic for one owning static and the inherited
//virtual whose dispatch slot it claims.
void ReportStaticVirtualCollision(SnClassDecl &super, const std::string &name,
	SnFunction &virtualMethod, SnFunction &own, BuildEnvironment &env)
{
	env.Log(CLL_Error, own.Location(),
		"The method \"%s\" claims the virtual dispatch slot of "
		"\"%s.%s\" with a different signature \"%s\". Align the "
		"signature to override, or rename the method.",
		own.Name().c_str(), super.Name().c_str(),
		FormatSignature(name, virtualMethod).c_str(),
		FormatSignature(name, own).c_str());
	env.Log(CLL_More, virtualMethod.Location(),
		"See also the declaration of \"%s.%s\".",
		super.Name().c_str(), FormatSignature(name, virtualMethod).c_str());
}

//A same-parameter-list static still claims the inherited virtual
//dispatch slot — the subclass declaration would look like an override
//yet never participate in dispatch — so the 0.9.7 rule stays for this
//one shape. Interface signatures are deliberately NOT checked here
//(D2.5: statics are contract-orthogonal).
bool CheckStaticVirtualCollision(SnClassDecl &cls,
	const std::string &name,
	const std::vector<SnFunction*> &funcs,
	BuildEnvironment &env)
{
	bool rejected = false;
	//One report per own method, anchored at the nearest ancestor (the
	//same winner dispatch order uses) — farther ancestors repeat the
	//same collision.
	std::set<SnFunction*> reported;
	std::set<SnClassDecl*> seen;
	for (auto *pSuper = cls.SuperClass(); pSuper && !seen.count(pSuper);
		pSuper = pSuper->SuperClass())
	{
		seen.insert(pSuper);
		if (pSuper->IsBuiltinClass())
			break;
		for (auto &member : pSuper->Members())
		{
			if (member.Kind() != NK_Function || member.Name() != name
				|| !member.ContainFlags(NF_Virtual)
				|| member.ContainFlags(NF_Static))
				continue;
			auto &virtualMethod = static_cast<SnFunction&>(member);
			for (auto *pOwn : funcs)
			{
				//ConflictedWith is the (name, parameter list) identity
				//shared with the duplicate-declaration check.
				if (!pOwn->ContainFlags(NF_Static)
					|| !pOwn->ConflictedWith(virtualMethod)
					|| reported.count(pOwn))
					continue;
				ReportStaticVirtualCollision(*pSuper, name, virtualMethod,
					*pOwn, env);
				reported.insert(pOwn);
				rejected = true;
			}
		}
	}
	return rejected;
}

} //namespace

//Per-name static-family rules (0.9.10 D2.4): a same-parameter-list
//static still collides with an inherited virtual method or with an
//Object protocol slot; every other static shape is contract-orthogonal.
bool CheckStaticNameRules(SnClassDecl &cls, const std::string &name,
	const std::vector<SnFunction*> &funcs, BuildEnvironment &env)
{
	if (CheckStaticVirtualCollision(cls, name, funcs, env))
		return true;
	return CheckObjectProtocol(cls, name, funcs, env);
}

} //namespace nlang
