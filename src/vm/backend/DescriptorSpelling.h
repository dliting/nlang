/*---
    DescriptorSpelling.h — the AST-side TypeArgDesc builder: turns an
    instantiation node's GenericTypeArgs() into descriptor arguments
    (canonical spelling + kind + payload). v0.13.1 runtime generic
    type information: consumed by the emission sites (OP_New/cast
    descriptor operands) and the module descriptor table.
---*/
#pragma once
#include "GenericInstDescriptor.h"
#include <cstdint>
#include <vector>

namespace nlang
{

class SnClassDecl;
class SnField;
class ModuleRegistry;

//The link identity one field keys the VM tables under — the same
//spelling Register.cpp writes class/struct/function records with.
//Exported so the descriptor spelling and the row key cannot drift
//(prelude members key bare, package members qualified, ownerless nodes
//degrade to the bare name). Defined in Register.cpp.
std::string LinkKeyOf(const ModuleRegistry& reg, const SnField& field);

//Type-parameter arguments are runtime-composable: rowRef encodes the
//parameter POSITION the argument reads from the receiver object's
//descriptor (the recipe detector below). Base keeps the plain -1 row
//domain disjoint from every legal row index.
constexpr int32_t kTypeParamArgBase = -2;
inline bool IsTypeParamArg(const TypeArgDesc& arg)
{ return arg.rowRef <= kTypeParamArgBase; }
inline int32_t TypeParamOrdinalOf(const TypeArgDesc& arg)
{ return kTypeParamArgBase - arg.rowRef; }

//Build the descriptor argument list of an instantiation node: each
//element of GenericTypeArgs() maps to one TypeArgDesc — scalars by
//canonical keyword, string as "string", class/interface/struct/enum by
//their link key (qualified for package members, bare for prelude and
//ownerless nodes), arrays as element spelling + rank marks with the
//element descriptor nested, nested instantiations recursively, and a
//type-parameter placeholder as the position encoding above (its
//spelling is the parameter name, diagnostic-only).
std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnClassDecl& inst);

//True when any argument is a type parameter — the instantiation's
//descriptor cannot be spelled at compile time and the emission must
//build it at runtime from the receiver (the recipe arm).
bool HasTypeParamArg(const std::vector<TypeArgDesc>& args);

} //namespace nlang
