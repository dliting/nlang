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
#include <string_view>

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
	//math — 25 functions. Types are exact; the only automatic promotion
	//is int->float widening (resolver wraps the argument in a cast).
	{"math", "sqrt",   {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Sqrt, false},
	{"math", "sin",    {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Sin, false},
	{"math", "cos",    {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Cos, false},
	{"math", "tan",    {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Tan, false},
	{"math", "asin",   {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Asin, false},
	{"math", "acos",   {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Acos, false},
	{"math", "atan",   {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Atan, false},
	//atan2 takes (y, x) in that order — same as C/C++ atan2.
	{"math", "atan2",  {RTK_Float, RTK_Float}, 2, 2, SLRT_Float, INTR_Math_Atan2, false},
	{"math", "pow",    {RTK_Float, RTK_Float}, 2, 2, SLRT_Float, INTR_Math_Pow, false},
	{"math", "exp",    {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Exp, false},
	{"math", "log",    {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Log, false}, //ln
	{"math", "absi",   {RTK_Int32}, 1, 1, SLRT_Int32, INTR_Math_Absi, false},
	{"math", "absf",   {RTK_Float}, 1, 1, SLRT_Float, INTR_Math_Absf, false},
	{"math", "mini",   {RTK_Int32, RTK_Int32}, 2, 2, SLRT_Int32, INTR_Math_Mini, false},
	{"math", "maxi",   {RTK_Int32, RTK_Int32}, 2, 2, SLRT_Int32, INTR_Math_Maxi, false},
	{"math", "minf",   {RTK_Float, RTK_Float}, 2, 2, SLRT_Float, INTR_Math_Minf, false},
	{"math", "maxf",   {RTK_Float, RTK_Float}, 2, 2, SLRT_Float, INTR_Math_Maxf, false},
	{"math", "clampi", {RTK_Int32, RTK_Int32, RTK_Int32}, 3, 3, SLRT_Int32, INTR_Math_Clampi, false},
	{"math", "clampf", {RTK_Float, RTK_Float, RTK_Float}, 3, 3, SLRT_Float, INTR_Math_Clampf, false},
	{"math", "floor",  {RTK_Float}, 1, 1, SLRT_Int32, INTR_Math_Floor, false},
	{"math", "ceil",   {RTK_Float}, 1, 1, SLRT_Int32, INTR_Math_Ceil, false},
	{"math", "round",  {RTK_Float}, 1, 1, SLRT_Int32, INTR_Math_Round, false},
	{"math", "random", {}, 0, 0, SLRT_Float, INTR_Math_Random, false},
	{"math", "srand",  {RTK_Int32}, 1, 1, SLRT_Void, INTR_Math_Srand, false},
	{"math", "randomi", {RTK_Int32, RTK_Int32}, 2, 2, SLRT_Int32, INTR_Math_Randomi, false},
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

//Table <-> id-block binding: every math entry carries an id inside the
//contiguous math block (CompiledModule.h), and the block has exactly one
//entry per id. A mismatch is only a runtime "unknown intrinsic" hole, so
//bind it here at compile time.
constexpr bool StdLibMathIdsInBlock()
{
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) != "math")
			continue;
		if (entry.intrinsicId < kMathIntrinsicFirst
			|| entry.intrinsicId >= kMathIntrinsicFirst + kMathIntrinsicCount)
			return false;
	}
	return true;
}
constexpr size_t StdLibMathEntryCount()
{
	size_t n = 0;
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) == "math")
			++n;
	}
	return n;
}
static_assert(StdLibMathIdsInBlock(),
	"math kStdLibTable entry points outside the contiguous intrinsic block");
static_assert(StdLibMathEntryCount() == kMathIntrinsicCount,
	"math kStdLibTable entry count must equal the intrinsic id block size");

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
