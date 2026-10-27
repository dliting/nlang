/*---
ModuleBuilderChecks.h - internal seam of the ModuleBuilder check
translation units: declarations shared between ModuleBuilderChecks.cpp
and the split check TUs (ModuleBuilderMethodContracts.cpp,
ModuleBuilderStaticNamespace.cpp).
Split from ModuleBuilderChecks.cpp (2026-10-17 maintainability
refactor, zero behavior change).
---*/
#pragma once
#include <string>
#include <vector>

namespace nlang
{

class SnNamespace;
class SnClassDecl;
class SnFunction;
class BuildEnvironment;

//Class collector shared by the circular-inheritance check and the
//method-contract driver (ModuleBuilderMethodContracts.cpp).
//Definition in ModuleBuilderChecks.cpp.
void CollectClasses(SnNamespace &root,
	std::vector<SnClassDecl*> &classes);

//Render a method signature for diagnostics; shared by the contracts
//and static-namespace TUs. Definition in ModuleBuilderChecks.cpp.
std::string FormatSignature(const std::string &name, const SnFunction &f);

//Per-name static-family rules of the method-contract family (definition
//in ModuleBuilderStaticNamespace.cpp): true when an own static claims an
//inherited virtual dispatch slot (0.9.10 D2.4) — the declared Object's
//protocol methods are inherited virtuals like any other since the
//builtin-decls migration.
bool CheckStaticNameRules(SnClassDecl &cls, const std::string &name,
	const std::vector<SnFunction*> &funcs, BuildEnvironment &env);

} //namespace nlang
