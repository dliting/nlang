/*---
StdLib.h — the single built-in table of the NLang standard library.

Defines which namespace-qualified functions (math.sqrt, io.print, ...)
exist, their parameter kinds, arity, return type and intrinsic id.
Shared by ExprResolver (call interception) and VmBackend (call emission).
Header-only constexpr data: both consumers live in libraries with a
one-way link (nlang_vm PRIVATE-links nlang_compiler), so a .cpp home on
either side would force a circular link.
---*/
#pragma once
#include "CompiledModule.h"  //RTK_* kind constants
#include <cstdint>
#include <string>

namespace nlang
{

//Return type of a stdlib function, resolved by the caller side to the
//matching language type (primitives via SnBuiltinDataType, List<string>
//via the generic class decl).
enum StdLibReturnType : uint8_t
{
	SLRT_Void = 0,
	SLRT_Int32,
	SLRT_Float,
	SLRT_String,
	SLRT_ListString,  //Step 3: s.split / fs.listFiles
};

//ABI note: namespace intrinsics differ from every other intrinsic family
//(BS/FS/List/Dict/Exception ctors all receive `this` at callParamBase[0]).
//Namespace functions are free functions: the VM reads the arguments from
//callParamBase slot 0 upward and there is no this pointer.
struct StdLibEntry
{
	const char* ns;      //"math" / "io" / "fs"
	const char* name;    //"sqrt" ...
	//Expected RTK_* of each parameter, in order. The parameter type
	//policy: exact kind match, or int->float widening (wrapped in a
	//cast expression by the resolver). Anything else is a compile error.
	uint8_t paramKinds[3];
	uint8_t minArgs;
	uint8_t maxArgs;
	uint8_t returnType;  //StdLibReturnType
	uint16_t intrinsicId;
	//Step 2 (io.print): accept string|int|float for every param and let
	//codegen convert int/float to string at the call site. Zero/false for
	//all other entries — they keep the strict paramKinds policy above.
	bool coerceToString;
};

//Whether name is one of the reserved stdlib namespaces ("math"/"io"/"fs").
//User declarations with these names are rejected at every registration
//point, so the name alone identifies a namespace-qualified call.
inline bool IsStdLibNamespaceName(const std::string& name)
{
	return name == "math" || name == "io" || name == "fs";
}

//The table itself (see the file-header note for why it is constexpr here).
inline constexpr StdLibEntry kStdLibTable[] =
{
	//math — Step 0 canary. Remaining 24 entries land with Step 1.
	{"math", "sqrt", {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Sqrt, false},
};

//Compile-time well-formedness: arity bounds must fit paramKinds[3] and
//not cross. Catches a bad Step 1+ table entry at compile time instead of
//as an out-of-bounds read in the resolver.
constexpr bool StdLibTableWellFormed()
{
	for (const auto& entry : kStdLibTable)
	{
		if (entry.minArgs > entry.maxArgs || entry.maxArgs > 3)
			return false;
	}
	return true;
}
static_assert(StdLibTableWellFormed(),
	"kStdLibTable entry has arity that does not fit paramKinds[3]");

//Look up a namespace-qualified function. Returns null when the namespace
//is known but the function is not (a distinct, diagnosable error).
inline const StdLibEntry* FindStdLibFunction(const std::string& ns,
	const std::string& name)
{
	if (!IsStdLibNamespaceName(ns))
		return nullptr;
	for (const auto& entry : kStdLibTable)
	{
		if (ns == entry.ns && name == entry.name)
			return &entry;
	}
	return nullptr;
}

} //namespace nlang
