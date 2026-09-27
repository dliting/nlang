/*---
StdLib.h — the runtime implementation table of the NLang standard library.

Maps each namespace-qualified function (math.sqrt, io.print, ...) to its
intrinsic id. The SIGNATURES (parameter kinds, arity, return type) are no
longer in this table: they live in the stdlib/*.n declarations and reach
the compiler and codegen through langservice::SymbolIndex. This table is
the last hardcoded piece of the standard library and will be replaced by
the native dynamic-loading mechanism (nlang_*.dll) in a later phase.
Header-only constexpr data: consumers live in libraries with a one-way
link (nlang_vm PRIVATE-links nlang_compiler), so a .cpp home on either
side would force a circular link.
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
	//Runtime implementation id. The signature (parameter names/kinds,
	//arity, return type) lives in the stdlib/*.n declarations and reaches
	//the compiler and codegen through langservice::SymbolIndex; this table
	//is only the ns,name -> intrinsicId implementation map, the last
	//hardcoded piece before native dynamic loading replaces it.
	uint16_t intrinsicId;
};

//Whether name is one of the reserved stdlib namespaces ("math"/"io"/"fs").
//User declarations with these names are rejected at every registration
//point, so the name alone identifies a namespace-qualified call.
inline bool IsStdLibNamespaceName(const std::string& name)
{
	return name == "math" || name == "io" || name == "fs";
}

//The table itself: ns,name -> intrinsicId (see the file-header note for
//why it is constexpr here). Signatures are NOT in this table anymore —
//they come from stdlib/*.n via langservice::SymbolIndex.
inline constexpr StdLibEntry kStdLibTable[] =
{
	//math — 25 functions.
	{"math", "sqrt",    INTR_Math_Sqrt},
	{"math", "sin",     INTR_Math_Sin},
	{"math", "cos",     INTR_Math_Cos},
	{"math", "tan",     INTR_Math_Tan},
	{"math", "asin",    INTR_Math_Asin},
	{"math", "acos",    INTR_Math_Acos},
	{"math", "atan",    INTR_Math_Atan},
	//atan2 takes (y, x) in that order — same as C/C++ atan2.
	{"math", "atan2",   INTR_Math_Atan2},
	{"math", "pow",     INTR_Math_Pow},
	{"math", "exp",     INTR_Math_Exp},
	{"math", "log",     INTR_Math_Log}, //ln
	{"math", "absi",    INTR_Math_Absi},
	{"math", "absf",    INTR_Math_Absf},
	{"math", "mini",    INTR_Math_Mini},
	{"math", "maxi",    INTR_Math_Maxi},
	{"math", "minf",    INTR_Math_Minf},
	{"math", "maxf",    INTR_Math_Maxf},
	{"math", "clampi",  INTR_Math_Clampi},
	{"math", "clampf",  INTR_Math_Clampf},
	{"math", "floor",   INTR_Math_Floor},
	{"math", "ceil",    INTR_Math_Ceil},
	{"math", "round",   INTR_Math_Round},
	{"math", "random",  INTR_Math_Random},
	{"math", "srand",   INTR_Math_Srand},
	{"math", "randomi", INTR_Math_Randomi},
	//io — content IO (console + text files). print is the one variadic-ish
	//'any' entry; readLine/readFile failures raise IOException at run time.
	{"io", "print",      INTR_Io_Print},
	{"io", "readLine",   INTR_Io_ReadLine},
	{"io", "readFile",   INTR_Io_ReadFile},
	{"io", "writeFile",  INTR_Io_WriteFile},
	{"io", "appendFile", INTR_Io_AppendFile},
	//fs — namespace/directory/metadata (never content). Mutations and
	//queries that cannot answer raise IOException at run time
	//(std::filesystem with error_code — no exceptions cross the ABI);
	//the three type predicates never raise: an un-statable path answers 0.
	{"fs", "exists",      INTR_FileSystem_Exists},
	{"fs", "isFile",      INTR_FileSystem_IsFile},
	{"fs", "isDirectory", INTR_FileSystem_IsDir},
	{"fs", "size",        INTR_FileSystem_Size},
	{"fs", "listFiles",   INTR_FileSystem_ListFiles},
	{"fs", "makeDirs",    INTR_FileSystem_MakeDirs},
	{"fs", "remove",      INTR_FileSystem_Remove},
	{"fs", "join",        INTR_FileSystem_Join},
};

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

//Same table <-> id-block binding for fs (Step 4).
constexpr bool StdLibFsIdsInBlock()
{
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) != "fs")
			continue;
		if (entry.intrinsicId < kFileSystemIntrinsicFirst
			|| entry.intrinsicId >= kFileSystemIntrinsicFirst
				+ kFileSystemIntrinsicCount)
			return false;
	}
	return true;
}
constexpr size_t StdLibFsEntryCount()
{
	size_t n = 0;
	for (const auto& entry : kStdLibTable)
	{
		if (std::string_view(entry.ns) == "fs")
			++n;
	}
	return n;
}
static_assert(StdLibFsIdsInBlock(),
	"fs kStdLibTable entry points outside the contiguous intrinsic block");
static_assert(StdLibFsEntryCount() == kFileSystemIntrinsicCount,
	"fs kStdLibTable entry count must equal the intrinsic id block size");

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
