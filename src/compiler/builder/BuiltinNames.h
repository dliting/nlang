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
#include <cstdint>
#include <string>

namespace nlang
{

class SnClassDecl;
struct SnMemberExpr;
struct SnInvokeExpr;

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

//Static faces on builtin generic TYPE NAMES — the type name as a call
//qualifier (`Future.selectAny(futures)`), no receiver instance: the
//intrinsic's arguments start at slot 0 (the binding row's noReceiver
//ABI). The resolver's chain-head arm gates + arity-checks from this
//table; the backend's emission reads the intrinsic id from the SAME
//row and never evaluates the qualifier.
struct BuiltinStaticFace
{
	const char* typeName;
	const char* methodName;
	uint16_t intrinsicId;
	size_t argCount;
};
const BuiltinStaticFace* FindBuiltinStaticFace(const std::string& typeName,
	const std::string& methodName);

//The static-face qualifier singleton: the arm binds the outer
//identifier of the member call to this decl (pointer identity is the
//codegen marker), keeping the tree fully resolved. MINTING version
//(resolver only). Deliberately outside IsBuiltinClassName: bare
//`Future` stays rejected in every type position (a generic type needs
//arguments) — the singleton exists solely as the call qualifier.
SnClassDecl* BuiltinStaticFaceClassDecl(const std::string& typeName);
//Non-minting probe (codegen): nullptr until the resolver minted one for
//this type name.
SnClassDecl* FindBuiltinStaticFaceClassDecl(const std::string& typeName);

} //namespace nlang
