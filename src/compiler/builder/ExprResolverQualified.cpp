/*---
    ExprResolverQualified.cpp - the qualified-name dispatch shared by every
    spelling of a module-qualified name (D-29 rule 1, ruling 45): where the
    module path ends, what an unimported path is blamed to, and the three
    qualified VALUE routes (a type's static member value, a module
    function value, a module type reference). Split from
    ExprResolverStdLib.cpp (2026-11-19 maintainability refactor, zero
    behavior change).
---*/
#include "ExprResolver.h"
#include "SnMisc.h"
#include "SnTypes.h"
#include "SnExtraTypes.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "ModuleRegistry.h"
#include <string>
#include <vector>

namespace nlang
{

//D-29 rule 1 (ruling 45): the shared answer to "where does the module path
//end", asked by every qualified-name route (the module-qualified call, the
//module type reference, the qualified member value) and by the declaration
//position in ExprResolverTypes.cpp — three arms each guessing it separately
//is how one spelling resolved for one kind of member and not for another.
//Known means a registered unit (a project module, an inlined library TU or
//an external .ncu, ModuleRegistry::IsKnownModule) or a library module the
//build can still name: one the symbol index knows or a search dir offers as
//a source (KnowsLibraryModule — the source arm is what makes a never-imported
//third-party submodule claimable, 0.13.12 Low 2). m12's type probe still
//only sees registered units — FindModuleType
//reads the declarations this build owns — so a library module that was never
//imported (hence never inlined) vetoes nothing: its chain is claimed and
//reported as unimported, which is the honest advice now that the wildcard
//import inlines it. rTailStart is the index of the first segment after the
//prefix, so it equals segs.size() when the whole chain is a module path.
bool ExprResolveAccessor::LongestKnownModulePrefix(
	const std::vector<std::string> &segs,
	std::string &rPrefix, size_t &rTailStart) const
{
	auto &reg = m_Env.Registry();
	for (size_t len = segs.size(); len > 0; --len)
	{
		const std::vector<std::string> head(segs.begin(),
			segs.begin() + len);
		const std::string prefix = JoinDots(head);
		if (!reg.IsKnownModule(prefix)
			&& !KnowsLibraryModule(prefix))
			continue;
		rPrefix = prefix;
		rTailStart = len;
		return true;
	}
	return false;
}

//D-29 rule 3 (ruling 45): the module path a diagnostic may blame as
//unimported. A chain that overshoots a LIBRARY module the build knows
//(`web.form.Form.f` with only `import web;`) is blamed on that deepest
//importable prefix — `import web.form.Form;` would itself be refused, and a
//remedy the user cannot apply is worse than none. A project child path that
//is not registered cannot be told from a typo here, so it keeps its exact
//name: its import then honestly answers `not found. Check the project
//Sources list or -I import path.`, the wording rule 3 reserves for module
//paths that really do not exist.
std::string ExprResolveAccessor::BlameModulePath(
	const std::vector<std::string> &segs) const
{
	if (segs.empty())
		return { };
	std::string prefix;
	size_t tailStart = 0;
	if (!LongestKnownModulePrefix(segs, prefix, tailStart)
		|| tailStart == segs.size()
		|| !KnowsLibraryModule(prefix))
		return JoinDots(segs);
	return prefix;
}

//D-29 rule 1 (ruling 45), the m12 tie: a known module-path prefix whose
//next segment names a type declared in that module captures the chain —
//a same-named type outranks a same-named child package at ANY depth, so
//a module claim must decline wherever the anchor sits (probing only the
//longest prefix let a depth-2 namesake claim what the type reading owns:
//with class helper in utils beside the submodule utils.helper, the chain
//utils.helper.answer has its type anchor at the SHALLOWER prefix). The
//scan stops at the first anchor, shallowest first — that is the boundary
//m12 has always decided. probeLimit bounds the segments that may
//capture: the call, function-value and type-reference routes pass their
//chain's end (any segment before the payload may end the path), the
//qualified type-value route passes its claim boundary so the Type.member
//tail it exists to bind is not a capture, and the declaration position
//passes its whole module path. FindModuleType reads this build's
//declarations, so an anchor in a never-imported library module captures
//nothing — that chain stays claimed and is reported as unimported, the
//same honesty LongestKnownModulePrefix documents above.
bool ExprResolveAccessor::TypeAnchorCapturesChain(
	const std::vector<std::string> &segs, size_t probeLimit,
	std::string *rpAnchorModule, std::string *rpAnchorType) const
{
	auto &reg = m_Env.Registry();
	std::string prefix;
	for (size_t len = 1; len < probeLimit && len < segs.size(); ++len)
	{
		prefix = len == 1 ? segs[0] : prefix + "." + segs[len - 1];
		if (!reg.IsKnownModule(prefix)
			&& !KnowsLibraryModule(prefix))
			continue;
		if (reg.FindModuleType(prefix, segs[len]) == nullptr)
			continue;
		if (rpAnchorModule != nullptr)
			*rpAnchorModule = prefix;
		if (rpAnchorType != nullptr)
			*rpAnchorType = segs[len];
		return true;
	}
	return false;
}

//Spec §7 row 1: the module is known but not imported into the current
//TU — name the fix and consume the chain here (normal resolution would
//only add "Cannot resolve the field" for the leftmost identifier).
//True = rejected and consumed; false = imported, processing continues.
bool ExprResolveAccessor::RejectUnimportedModuleCall(
	SnMemberExpr &snMember, const std::string &modulePath)
{
	auto &reg = m_Env.Registry();
	const uint32_t owner = reg.OwnerOfContext(*m_pContext);
	//The chain's own module path can overshoot the module the user has to
	//import (`web.form.Form.f` with `import web;` — the class is a member of
	//the library module `web.form`, not of a module named `web.form.Form`),
	//so the gate question and the blamed name are the folded prefix. A
	//prefix that is in fact imported means the import is right and the
	//member is wrong: decline and let the member lookup name it.
	const std::string blamed = BlameModulePath(SplitDots(modulePath));
	//Single-point import-gate policy (ModuleRegistry::ImportGateAllows):
	//stub-synthesized calls (external owner) bypass the consumer gate;
	//source calls must have the module in this TU's gate.
	if (reg.ImportGateAllows(owner, blamed))
		return false;
	if (blamed.find('.') == std::string::npos)
	{
		//Single-segment path. A library package (io/math/fs or a
		//third-party package) reads "Package"; an external .ncu reads
		//"Module". No wildcard is suggested — it never matches a
		//single-segment name (§3.3); the exact form always suffices.
		const char *pKind = KnowsLibraryModule(blamed)
			? "Package" : "Module";
		m_Env.Log(CLL_Error, snMember.Location(),
			"%s '%s' is not imported. Add 'import %s;' at the top "
			"of this file.", pKind, blamed.c_str(), blamed.c_str());
	}
	else
	{
		const size_t lastDot = blamed.find_last_of('.');
		const std::string parentPrefix =
			blamed.substr(0, lastDot);
		m_Env.Log(CLL_Error, snMember.Location(),
			"Module '%s' is not imported. Add 'import %s;' (or "
			"'import %s.*;') at the top of this file.",
			blamed.c_str(), blamed.c_str(), parentPrefix.c_str());
	}
	FinishConsumedMember(snMember);
	return true;
}

//Shared prologue of the qualified VALUE routes (the qualified member
//value, the free-function value and the module type reference): the
//dotted chain with the tip pushed back, then the m12 priority probe —
//the leftmost local/field/type wins, as for calls. False = declined,
//the generic path keeps the expression.
bool ExprResolveAccessor::RouteQualifiedValueChain(
	SnMemberExpr &snMember, std::vector<std::string> &segs)
{
	segs = OuterIdentifierChain(snMember);
	if (segs.empty()
		|| !snMember.Inner()
		|| snMember.Inner()->Kind() != NK_IdentifierExpr)
		return false;
	segs.push_back(
		static_cast<SnIdentifierExpr *>(snMember.Inner())->Name());

	//m12 priority: the leftmost local/field/type wins (as for calls).
	if (ProbeNonFunctionField(segs.front())
		|| IsBuiltinClassName(segs.front()))
		return false;
	return true;
}

//The not-a-member wording shared by the qualified-type admission sites
//(0.13.8 item 5): the literal lives in one place so the channels that
//fold a foreign non-public type into not-found cannot drift apart.
void ExprResolveAccessor::LogModuleTypeNotFound(SyntaxNode &site,
	const std::string &typeName, const std::string &modulePath)
{
	m_Env.Log(CLL_Error, site.Location(),
		"Type '%s' is not a member of package '%s'.",
		typeName.c_str(), modulePath.c_str());
}

//The type admission of the qualified VALUE routes (0.13.8 item 5,
//Task 4): the foreign non-public arm folds into not-found — the
//not-a-member wording covers both shapes and the member consumes on
//the miss. Null = reported and consumed; non-null = the admitted type.
SnField *ExprResolveAccessor::FindAdmittedModuleTypeOrReport(
	SnMemberExpr &snMember, const std::string &modulePath,
	const std::string &typeName)
{
	auto &reg = m_Env.Registry();
	bool bDenied = false;
	SnField *pType = FindAdmittedModuleType(reg, modulePath, typeName,
		reg.OwnerOfContext(snMember), bDenied);
	if (pType != nullptr)
		return pType;
	LogModuleTypeNotFound(snMember, typeName, modulePath);
	FinishConsumedMember(snMember);
	return nullptr;
}

//D-29 rule 1 (ruling 45), Phase 5: package-qualified VALUE access — the
//`alib.Color.Green` enum-constant chain and the `json.JsonValue.MAX_DEPTH`
//class-static chain alike, resolved through the registry (see the header
//declaration for the route contract). Every member kind binds here: the route
//admitted only enum decls, which pushed a class-static chain back onto the
//module-path arms — and they named the type as the problem for a missing
//member. Admission belongs to the same rule: the qualified spelling may bind
//only what the bare spelling may bind, so the found member goes through the
//same cross-module gate the field and method lookups use. A miss — absent or
//not admitted — declines so the generic member path names the member itself
//with the wording the bare spelling gets. True = consumed (resolved or
//diagnosed); false = declined, the generic path keeps the expression.
bool ExprResolveAccessor::TryResolveQualifiedTypeValue(SnMemberExpr &snMember)
{
	std::vector<std::string> segs;
	if (!RouteQualifiedValueChain(snMember, segs))
		return false;

	//The module path ends at the longest known prefix, and this route owns
	//the chain only when exactly `Type.member` is left of it: a longer tail
	//is a deeper member chain (the recursion binds its own head, so
	//`json.JsonValue.MAX_DEPTH` reaches the type-receiver path), a shorter
	//one a module function value or a bare type reference, and each of those
	//has its own route.
	std::string modulePath;
	size_t tailStart = 0;
	if (!LongestKnownModulePrefix(segs, modulePath, tailStart)
		|| segs.size() - tailStart != 2)
		return false;
	//The m12 tie (ruling 45): a type anchored at a shallower known prefix
	//captures the chain before this route's claim prefix — utils.helper
	//the class shadows utils.helper the submodule, so the Type.member tail
	//binds in the class's scope through the generic path, not the
	//submodule's here. The tail itself never captures: its head IS the
	//type this route exists to bind.
	if (TypeAnchorCapturesChain(segs, tailStart))
		return false;
	const std::string &typeName = segs[tailStart];
	const std::string &memberName = segs.back();

	if (RejectUnimportedModuleCall(snMember, modulePath))   //import gate, call wording
		return true;

	SnField *pType = FindAdmittedModuleTypeOrReport(
		snMember, modulePath, typeName);
	if (pType == nullptr)
		return true;

	//Every member kind binds, behind the same cross-module access gate as the
	//bare spelling (D-29 rule 1); an absent or unadmitted member declines.
	SnField *pMember = pType->FindField(memberName);
	if (pMember == nullptr
		|| !AccessAdmitted(m_Env.Registry(), *pMember, *m_pAccessor))
		return false;

	//Bind tip + member exactly like FinishResolvedMember does.
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Inner()),
		pMember);
	ResolveFieldExprAs(snMember, pMember);
	FinishConsumedMember(snMember);
	return true;
}

//v2.2: module-qualified free-function VALUE reference — `lib.seven`,
//where the tip names a function of an imported module (the value form of
//TryResolveModuleQualified's call form; signatures serialize since the
//func TypeDesc grammar). The chain resolves into the same pending state
//as a bound method reference, so every shared bind site (assign /
//init-list / invoke-argument matching) lights up unchanged. D-29 rule 1
//binds admission as well as lookup, so the candidates go through the same
//access gate the qualified CALL form has (ResolveModuleQualifiedCallee's
//RejectInaccessibleCallee); without it a private module function is
//referenceable through the qualified spelling alone. The gate is asked after
//the import check for the same reason the call route asks it there — an
//unimported module is the importable name's problem before it is an access
//problem — and no admitted candidate declines, so the generic member path
//reports the member as the bare spelling does. True = consumed (resolved or
//diagnosed); false = declined — the tip is no accessible function of the
//module and the package-value / generic paths keep the chain.
bool ExprResolveAccessor::TryResolveModuleFuncValue(SnMemberExpr &snMember)
{
	std::vector<std::string> segs;
	if (!RouteQualifiedValueChain(snMember, segs))
		return false;

	//The m12 tie (ruling 45): a type anchored anywhere before the
	//function tip captures the chain, so the value spelling cannot reach
	//a submodule function the call form would shadow.
	if (TypeAnchorCapturesChain(segs, segs.size()))
		return false;

	const std::string funcName = segs.back();
	segs.pop_back();
	std::string modulePath = JoinDots(segs);

	//Candidates BEFORE the import gate: a chain whose middle segment is a
	//function, not a module (io.print.nope), finds no functions here and
	//declines — the package-value / generic paths keep it and report the
	//real member diagnostic. Only a resolved function reference of a
	//registered-but-unimported module reaches the import wording below.
	auto candidates = m_Env.Registry().ModuleFunctions(modulePath, funcName);
	if (candidates.empty())
		return false;

	if (RejectUnimportedModuleCall(snMember, modulePath))   //import gate, call wording
		return true;

	SnFunction *pFunc = nullptr;
	for (SnFunction *pCand : candidates)
	{
		if (AccessAdmitted(m_Env.Registry(), *pCand, *m_pAccessor))
		{
			pFunc = pCand;
			break;
		}
	}
	if (pFunc == nullptr)
		return false;

	//Bind tip + member exactly like FinishResolvedMember does — the
	//pending state carries the function's return type until an expected
	//func type binds the reference (TryBindStatementFuncRef, invoke
	//argument matching). Overloads resolve like the unqualified-name form: the
	//first admitted declaration under the name.
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Inner()), pFunc);
	ResolveFieldExprAs(snMember, pFunc);
	FinishConsumedMember(snMember);
	return true;
}

//0.9.12 D4: module-qualified TYPE reference — `lib.Mix` (module path +
//type name, any nesting depth) in expression position. The registry
//binds the unit's type declaration (project, inline library or external
//.ncu stub alike), so the generic type-receiver path then resolves the
//member or static call in the type scope — the qualified spelling shares
//the stub record and the diagnostics of the bare `Mix` form. True =
//consumed (resolved or diagnosed); false = declined — the chain is not a
//module path or the leftmost name belongs to something else, and the
//generic path keeps it.
bool ExprResolveAccessor::TryResolveModuleTypeRef(SnMemberExpr &snMember)
{
	std::vector<std::string> segs;
	if (!RouteQualifiedValueChain(snMember, segs))
		return false;

	std::string typeName = segs.back();
	segs.pop_back();

	//D-29 rule 1 (ruling 45): this is a module type reference only when
	//everything before the type name is itself a module path — the longest
	//known prefix has to reach the type name. `json.JsonValue.MAX_DEPTH`
	//fails that (its prefix stops at `json`), so the route declines, the
	//recursion binds the shorter chain, and the member resolves in the
	//type's scope instead of this arm claiming the whole chain as a module.
	//The m12 tie reaches the middle segments the same way: a namesake type
	//before the module path captures the chain and the generic path binds
	//the shorter head. The type tip is never probed — a type can only live
	//in a module, so the module reading of the tip has no type-reading
	//rival to lose to.
	std::string modulePath;
	size_t tailStart = 0;
	if (TypeAnchorCapturesChain(segs, segs.size())
		|| !LongestKnownModulePrefix(segs, modulePath, tailStart)
		|| tailStart != segs.size())
		return false;

	if (RejectUnimportedModuleCall(snMember, modulePath))   //import gate, call wording
		return true;

	SnField *pType = FindAdmittedModuleTypeOrReport(
		snMember, modulePath, typeName);
	if (pType == nullptr)
		return true;

	//Bind tip + member exactly like FinishResolvedMember does — the
	//member becomes a type expression and SwitchContextToReceiver lands
	//the inner search in the type's scope (the bare-form route).
	ResolveFieldExprAs(static_cast<SnFieldExpr &>(*snMember.Inner()), pType);
	ResolveFieldExprAs(snMember, pType);
	FinishConsumedMember(snMember);
	return true;
}

} //namespace nlang
