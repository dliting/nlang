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
	//io — content IO (console + text files). print is the one coercing
	//entry; the file trio is strictly (string, string) and readLine/readFile
	//failures raise IOException at run time.
	{"io", "print",      {RTK_String}, 1, 1, SLRT_Void,   INTR_Io_Print,      true},
	{"io", "readLine",   {},           0, 0, SLRT_String, INTR_Io_ReadLine,   false},
	{"io", "readFile",   {RTK_String}, 1, 1, SLRT_String, INTR_Io_ReadFile,   false},
	{"io", "writeFile",  {RTK_String, RTK_String}, 2, 2, SLRT_Void, INTR_Io_WriteFile,  false},
	{"io", "appendFile", {RTK_String, RTK_String}, 2, 2, SLRT_Void, INTR_Io_AppendFile, false},
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

//Same table <-> id-block binding for io (Step 2).
constexpr bool StdLibIoIdsInBlock()
{
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) != "io")
			continue;
		if (entry.intrinsicId < kIoIntrinsicFirst
			|| entry.intrinsicId >= kIoIntrinsicFirst + kIoIntrinsicCount)
			return false;
	}
	return true;
}
constexpr size_t StdLibIoEntryCount()
{
	size_t n = 0;
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) == "io")
			++n;
	}
	return n;
}
static_assert(StdLibIoIdsInBlock(),
	"io kStdLibTable entry points outside the contiguous intrinsic block");
static_assert(StdLibIoEntryCount() == kIoIntrinsicCount,
	"io kStdLibTable entry count must equal the intrinsic id block size");

//Built-in string methods (Step 3): receiver-dispatched, NOT namespace
//calls — s.substring(1) resolves in the string-method branch of
//Access(SnMemberExpr) and emits with the receiver at callParamBase[0]
//and args from slot 1 (mirror of string.equals). The method surface is
//frozen as the future string class's method list (user decision #6).
//Optional trailing-argument default for a string method. OP_CallIntrinsic
//carries no argument count, so a shorter call would leave stale memory in
//the trailing callParamBase slot — the missing argument must be staged
//synthetically. Single value today: substring's end defaults to the
//receiver's length (staged via OP_StrLen on the receiver copy).
enum StringTrailingDefault : uint8_t
{
	STD_None = 0,
	STD_ReceiverLength,
};

struct StringMethodEntry
{
	const char* name;
	//Expected RTK_* of each parameter in order (substring takes byte
	//offsets; everything else takes strings). Exact kind match only.
	//Fixed size 2 — the method surface is frozen by decision #6, so no
	//entry may take a 3rd param; a zero-init slot would read as
	//RTK_Int32, so keep maxArgs <= 2 when extending the table.
	uint8_t paramKinds[2];
	uint8_t minArgs;
	uint8_t maxArgs;
	uint8_t returnType;  //StdLibReturnType
	uint16_t intrinsicId;
	//Stage missing trailing args when argCount < maxArgs (codegen emits
	//the synthesized value; the walker reserves the extra slot — both
	//consume this same field, keeping the two sides symmetric).
	uint8_t trailingDefault;  //StringTrailingDefault
};

inline constexpr StringMethodEntry kStringMethodTable[] =
{
	{"substring",  {RTK_Int32, RTK_Int32}, 1, 2, SLRT_String,     INTR_String_Substring,  STD_ReceiverLength},
	{"indexOf",    {RTK_String},           1, 1, SLRT_Int32,      INTR_String_IndexOf,    STD_None},
	{"startsWith", {RTK_String},           1, 1, SLRT_Int32,      INTR_String_StartsWith, STD_None},
	{"endsWith",   {RTK_String},           1, 1, SLRT_Int32,      INTR_String_EndsWith,   STD_None},
	{"contains",   {RTK_String},           1, 1, SLRT_Int32,      INTR_String_Contains,   STD_None},
	{"toUpper",    {},                     0, 0, SLRT_String,     INTR_String_ToUpper,    STD_None},
	{"toLower",    {},                     0, 0, SLRT_String,     INTR_String_ToLower,    STD_None},
	{"trim",       {},                     0, 0, SLRT_String,     INTR_String_Trim,       STD_None},
	{"split",      {RTK_String},           1, 1, SLRT_ListString, INTR_String_Split,      STD_None},
	{"replace",    {RTK_String, RTK_String}, 2, 2, SLRT_String,   INTR_String_Replace,    STD_None},
	{"toInt",      {},                     0, 0, SLRT_Int32,      INTR_String_ToInt,      STD_None},
	{"toFloat",    {},                     0, 0, SLRT_Float,      INTR_String_ToFloat,    STD_None},
};

//Table <-> id-block binding for the string-method family.
constexpr bool StringMethodIdsInBlock()
{
	for (const auto& entry : kStringMethodTable)
	{
		if (entry.intrinsicId < kStringMethodIntrinsicFirst
			|| entry.intrinsicId >= kStringMethodIntrinsicFirst
				+ kStringMethodIntrinsicCount)
			return false;
	}
	return true;
}
constexpr size_t StringMethodEntryCount()
{
	size_t n = 0;
	for (const auto& entry : kStringMethodTable)
		++n;
	return n;
}
static_assert(StringMethodIdsInBlock(),
	"string-method table entry points outside the contiguous intrinsic block");
static_assert(StringMethodEntryCount() == kStringMethodIntrinsicCount,
	"string-method table entry count must equal the intrinsic id block size");

//Look up a built-in string method by name (null when unknown).
inline const StringMethodEntry* FindStringMethod(const std::string& name)
{
	for (const auto& entry : kStringMethodTable)
	{
		if (name == entry.name)
			return &entry;
	}
	return nullptr;
}

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
