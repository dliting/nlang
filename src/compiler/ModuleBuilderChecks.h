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
class SnField;
class SnFieldExpr;
class SnFunction;
class BuildEnvironment;
//struct (not class): MSVC decorates the two differently, so the
//forward declaration here must match the real declaration in
//TypeDef.h or the symbol mangles apart from its callers.
struct ISourceLocation;

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

//Dictionary-model contract for generic class declarations (definition
//in GenericDeclContracts.cpp): members mix freely — bodies, fields and
//native members in one template row. Rejected: a member neither native
//nor with a body, and initializers/default values on bare
//type-parameter-typed members (no concrete kind exists for either).
void CheckGenericDeclContract(SnClassDecl &cls, BuildEnvironment &env);

//Null unless the declared type is a bare type-parameter reference (T,
//not T[] or List<T>); returns the parameter name (definition in
//GenericDeclContracts.cpp). Shared by the class contract and the
//interface member contract (0.13.1c) — the interface gates read the
//same placeholder binding the class gates do.
const std::string *BareTypeParamName(const SnFieldExpr *pType,
	BuildEnvironment &env);

//0.13.1b: declaration-time cap on a generic declaration's type
//parameters (definition in GenericDeclContracts.cpp) — ordinals are
//0-based and the typeName/T-equality ordinal byte reserves its top
//value (0xFF) as the no-ordinal marker, so a 256th parameter would
//collide with the sentinel. Shared by the class path and the free
//function path.
void CheckGenericTypeParamCount(const std::vector<std::string> &names,
	const std::string &declName, const ISourceLocation *pLoc,
	BuildEnvironment &env);

//v0.12.3: reject arrays of a type parameter at the array-token intern
//entry points (definition in GenericDeclContracts.cpp) — the element
//kind is not representable in the template's shared row, and a token
//interned over the placeholder would fail the registration walk with
//a backend internal error. True = rejected (rule diagnostic logged).
bool RejectTypeParamArrayElement(SnField *pElemField,
	BuildEnvironment &env, const ISourceLocation *pLoc);

} //namespace nlang
