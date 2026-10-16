/*---
ModuleBuilderChecks.h - internal seam of the ModuleBuilder check
translation units: declarations shared between ModuleBuilderChecks.cpp
and the split check TUs (ModuleBuilderMethodContracts.cpp,
ModuleBuilderObjectProtocol.cpp). Split from ModuleBuilderChecks.cpp
(2026-10-17 maintainability refactor, zero behavior change).
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
//TU and the Object-protocol TU. Definition in
//ModuleBuilderMethodContracts.cpp.
std::string FormatSignature(const std::string &name, const SnFunction &f);

//Object-protocol check of the method-contract family (definition in
//ModuleBuilderObjectProtocol.cpp): true when any own same-name method
//of cls mismatches the implicit Object contract (equals/getHashCode/
//toString, spec D4).
bool CheckObjectProtocol(SnClassDecl &cls, const std::string &name,
	const std::vector<SnFunction*> &funcs, BuildEnvironment &env);

} //namespace nlang
