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
//runtime class names of List/Dict/Channel; func has no runtime class —
// its values are RTK_Func records).
inline constexpr const char* kBuiltinListTypeName = "List";
inline constexpr const char* kBuiltinDictTypeName = "Dict";
inline constexpr const char* kBuiltinFuncTypeName = "func";
inline constexpr const char* kBuiltinChannelTypeName = "Channel";
inline constexpr const char* kBuiltinFutureTypeName = "Future";
//Promise is a DECLARED generic (stdlib/builtins.n template + hidden
//__handle slot + intrinsics), NOT a member of the synthetic-minting
//whitelist below: every consumer of IsBuiltinGenericTypeName routes on
//it between the synthetic and the declared-template resolution paths,
//and Promise must take the declared path (its members are substituted
//from the AST template). The constant exists for the handle-class name
//table and the executor's erased-class cache.
inline constexpr const char* kBuiltinPromiseTypeName = "Promise";

//Returns true if name is a recognized built-in generic type name.
//The set is the SCANNER's generic-open flag set only: a '<' directly
//after one of these names is a type-argument opener, and it keeps the
//">>" split depth counting working for the four names. Since the
//phase-2 source-of-truth migration the resolver routes ONLY func to
//synthetic minting — List/Dict/Channel/Future resolve through their
//declared templates in stdlib/builtins.n, exactly like Promise and
//every user `class C<T>` (no lex-time set can know a module's class
//declarations, so declared use sites parse on the grammar's plain-'<'
//type arms).
inline bool IsBuiltinGenericTypeName(const std::string& name)
{
	return name == kBuiltinListTypeName || name == kBuiltinDictTypeName
		|| name == kBuiltinFuncTypeName
		|| name == kBuiltinChannelTypeName
		|| name == kBuiltinFutureTypeName;
}

} //namespace nlang
