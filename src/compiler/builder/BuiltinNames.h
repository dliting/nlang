/*-----------------------------------------------------------------------------
	ncomp/builder/BuiltinNames.h
	This file defines the shared name predicates for the lazily synthesized
built-in classes (ByteStream, FileStream). ExprResolver
owns the synthesis sites; DuplicateFieldChecker consults the same
predicates for the Phase 13 type-alias clash check, so both must agree on
one name set. The Exception family, Mutex/Semaphore, Thread and Object
left the synthesized set — declared in stdlib/builtins.n (builtin-decls
source of truth), resolving through the scope chain like any class.
-----------------------------------------------------------------------------*/
#pragma once
#include <string>

namespace nlang
{

//Phase 10 audit H2: single predicate for every name GetBuiltinClassDecl
//can synthesize. Call sites used to duplicate this filter three times —
//a future builtin added to one copy but not the chain below would fall
//into the wrong arm and silently corrupt the singleton. Everything
//routes through this one function now.
//Phase 5: the bare-name comparison stays — builtins carry no owner tag,
//so their table keys stay bare; a user `pkg.Object` never matches here.
//K1 Slice 9: Channel is generic-only (Channel<T>); the element type
//decides boxing, so the bare name is not a type. The builtin-decls
//source-of-truth migration removed Semaphore/Mutex (cluster 1), Thread
//(cluster 3) and Object (cluster 4) — their classes are declared in
//stdlib/builtins.n and resolve through the scope chain like any
//imported class.
inline bool IsBuiltinClassName(const std::string& name)
{
	return name == "ByteStream" || name == "FileStream";
}

} //namespace nlang
