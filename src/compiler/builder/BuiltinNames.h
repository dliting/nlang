/*-----------------------------------------------------------------------------
	ncomp/builder/BuiltinNames.h
	This file defines the shared name predicates for the lazily synthesized
built-in classes (ByteStream, FileStream, Object, the Exception hierarchy).
ExprResolver owns the synthesis sites; DuplicateFieldChecker consults the
same predicates for the Phase 13 type-alias clash check, so both must agree
on one name set.
-----------------------------------------------------------------------------*/
#pragma once
#include <string>

namespace nlang
{

//Phase 9d: returns true for any name in the built-in Exception hierarchy.
inline bool IsBuiltinExceptionClassName(const std::string& name)
{
	return name == "Exception" || name == "NullPointerException"
		|| name == "DivByZeroException" || name == "IndexOutOfBoundsException"
		|| name == "AssertionException" || name == "IOException"
		|| name == "InvalidCastException";
}

//Phase 10 audit H2: single predicate for every name GetBuiltinClassDecl
//can synthesize. Call sites used to duplicate this filter three times —
//a future builtin added to one copy but not the chain below would fall
//into the old `s_pObjectClass` fallback and silently corrupt the Object
//singleton. Everything routes through this one function now.
//Phase 5: the bare-name comparison stays — builtins carry no owner tag,
//so their table keys stay bare; a user `pkg.Object` never matches here.
//K1: the concurrency builtins Thread/Mutex join the set (the lowercase
//`thread` NAMESPACE is a different surface — ThreadNamespace.h). K1
//Slice 9: Channel left the set — it is generic-only (Channel<T>); the
//element type decides boxing, so the bare name is not a type.
inline bool IsBuiltinClassName(const std::string& name)
{
	return name == "ByteStream" || name == "FileStream" || name == "Object"
		|| name == "Thread" || name == "Mutex"
		|| IsBuiltinExceptionClassName(name);
}

} //namespace nlang
