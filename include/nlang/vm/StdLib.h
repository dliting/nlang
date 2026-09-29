/*---
StdLib.h — the string-method intrinsic table.

Maps a string method name (substring, split, ...) to its intrinsic id. The
SIGNATURES live in stdlib/*.n declarations and reach codegen through
langservice::SymbolIndex. Everything else the standard library used to
hardcode here (the math/io/fs signature table) is gone: those namespaces
are ordinary library sources whose native members are served by
nlang_<ns>.dll through the NativeHost ABI, exactly like a third-party
package.
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

//Return type of a built-in string method, resolved by the caller side to
//the matching language type (primitives via SnBuiltinDataType, List<string>
//via the generic class decl).
enum StdLibReturnType : uint8_t
{
	SLRT_Void = 0,
	SLRT_Int32,
	SLRT_Float,
	SLRT_String,
	SLRT_ListString,  //s.split
};

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

} //namespace nlang
