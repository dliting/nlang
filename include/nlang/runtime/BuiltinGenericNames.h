/*-----------------------------------------------------------------------------
	include/nlang/runtime/BuiltinGenericNames.h
	The source-level type names of the built-in generics (List, Dict, func).

	The erasure design makes each spelling one identity: the type name
	written in NLang source (List<...>, Dict<...>, func<...>) is the same
	string the VM registers its erased shared backing classes under
	(src/vm/backend/BuiltinClasses.cpp), the same string the compiler
	mints base-name nodes from (MakeFuncType in the grammar), and the same
	string the embed adapter matches against the runtime class name to
	tell List/Dict instances apart from user classes. Every site that
	mints a base name, compares a BaseName, or looks up an erased class
	must use these constants — one spelling, one identity.

	Distinct from the RTK_* kind labels ("func"/"Func" in the
	disassembler and debug printer) and from the host Value kind names
	(src/embed/Value.cpp): those are display names of runtime kinds, not
	type names.
-----------------------------------------------------------------------------*/
#pragma once
#include <string>

namespace nlang
{
//Source-level type names of the built-in generics (also the erased
//runtime class names of List/Dict; func has no runtime class — its
//values are RTK_Func records).
inline constexpr const char* kBuiltinListTypeName = "List";
inline constexpr const char* kBuiltinDictTypeName = "Dict";
inline constexpr const char* kBuiltinFuncTypeName = "func";

//Returns true if name is a recognized built-in generic type name.
//Phase 8e-3: List (arity 1). Phase 8e-4: Dict (arity 2). Phase 13:
//func (variadic, at least the return type); 0.8.3 renamed the func form
//to the lowercase keyword spelling.
inline bool IsBuiltinGenericTypeName(const std::string& name)
{
	return name == kBuiltinListTypeName || name == kBuiltinDictTypeName
		|| name == kBuiltinFuncTypeName;
}

} //namespace nlang
