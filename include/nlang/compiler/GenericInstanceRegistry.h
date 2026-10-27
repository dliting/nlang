/*---
GenericInstanceRegistry.h - declared-generic placeholder and
instantiation registries.

Compile-time machinery for `class Name<T, ...>` declarations: the
template resolves its member signatures against interned placeholder
types (one per type parameter), and each `Name<Arg...>` use mints a
synthetic instantiation whose members are signature deep-copies with
placeholders substituted. All artifacts are compile-time only: the
instantiations never enter the syntax tree; the VM backend reads them
through the accessors below (per-unit registration and method-table
population).
---*/
#ifndef NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
#define NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H

#include <functional>
#include <string>
#include <vector>

namespace nlang
{

class SnClassDecl;
class SnField;

//The template's placeholder for the type parameter `name` at parameter
//position `position` (interned on first request; one node per
//(template, parameter) pair).
SnField* FindTypeParamPlaceholder(SnClassDecl& tmpl, const std::string& name,
	size_t position);

//Is this field one of the interned type-parameter placeholders?
bool IsTypeParamPlaceholder(const SnField* pField);

//The parameter position a placeholder stands for (an index into the
//template's TypeParamNames()). Precondition: IsTypeParamPlaceholder.
size_t TypeParamIndexOf(const SnField* pPlaceholder);

//The template a declared instantiation was minted from; null when the
//field is not a declared-generic instantiation.
SnClassDecl* DeclaredInstanceOf(const SnClassDecl* pInstance);

//Iterate the declared instantiations minted while resolving `ownerUnit`,
//in mint order. The VM backend calls this per unit image to register
//the instantiation classes and their member records.
void ForEachDeclaredGenericInstance(uint32_t ownerUnit,
	const std::function<void(SnClassDecl&)>& visitor);

} //namespace nlang

#endif //NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
