/*-----------------------------------------------------------------------------
	ncomp/builder/BuiltinNames.h
	This file defines the shared name predicates for the lazily synthesized
built-in classes (ByteStream, FileStream). ExprResolver
owns the synthesis sites; DuplicateFieldChecker consults the same
predicates for the Phase 13 type-alias clash check, so both must agree on
one name set. The Exception family and Object left the synthesized set —
declared in stdlib/lang.n (builtin-decls source of truth), resolving
through the scope chain like any class. The concurrency handles moved on
to stdlib/thread.n and stdlib/concurrent.n (v0.12.3 import boundary):
same declared shape, but they enter a translation unit only through its
import.
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
//(cluster 3) and Object (cluster 4); the v0.12.3 import boundary then
//moved Semaphore/Mutex and Thread to the concurrency packages
//(stdlib/concurrent.n, stdlib/thread.n). Object stayed in the
//stdlib/lang.n prelude; all of them resolve through the scope
//chain like any declared class.
//v0.12.7 Task 8.5: the stream classes moved to stdlib/io.n (real
//class declarations + binding-table stamping, the List/Dict pattern).
//No synthesized builtin class remains, so the predicate answers false
//for every name and its call sites fall through to the ordinary
//declared-class resolution. Retained (not deleted) while the retired
//synthetic-singleton machinery is cleaned up file by file.
inline bool IsBuiltinClassName(const std::string& name)
{
	(void)name;
	return false;
}

//The stream class names — declared in stdlib/io.n since Task 8.5. The
//resolver's literal-type special path (readStruct/readObject) and the
//struct/object argument gates (writeStruct/writeObject) key on these
//names; everything else about the classes is ordinary declaration
//resolution.
inline bool IsStreamClassName(const std::string& name)
{
	return name == "ByteStream" || name == "FileStream";
}

//v0.12.2 face classes (spec D2/D10): String/Array hold the method faces
//of the string keyword and the array types. Shared by the resolver gates,
//the implicit-Object early-out, and the root mirror name-yield pass
//(ImportedNodeBuilder) — one set, no drift.
inline bool IsPreludeFaceClassName(const std::string& name)
{
	return name == "String" || name == "Array";
}

//v0.12.6 (spec §6.2 D2): name of the synthesized lazy static
//initializer every class with an initialized static (own or inherited)
//carries as a member. `$` cannot appear in a source identifier, so the
//name alone identifies the member. Shared by the resolver synthesis,
//the class method-table builder (skips it — a consumer stub must not
//rebuild it as a callable ghost method — while recording its index as
//staticInitFuncIdx) and the backend's special-case emission.
inline constexpr const char* kStaticInitFuncName = "$staticInit";

inline bool IsStaticInitName(const std::string& name)
{
	return name == kStaticInitFuncName;
}

//Static faces on builtin generic TYPE NAMES — the type name as a call
//qualifier (`Future.selectAny(futures)`), no receiver instance: the
//intrinsic's arguments start at slot 0 (the binding row's noReceiver
//ABI). The resolver's chain-head arm gates + arity-checks from this
//table; the backend's emission reads the intrinsic id from the SAME
//row and never evaluates the qualifier.
struct BuiltinStaticFace
{
	const char* package;      //declaring package (binding-row key)
	const char* typeName;
	const char* methodName;
	uint16_t intrinsicId;
	size_t argCount;
};
const BuiltinStaticFace* FindBuiltinStaticFace(const std::string& typeName,
	const std::string& methodName);
//Every face row — the single source the binding table derives its
//static-face rows from (one face, one ABI, no second hand-maintained
//copy to drift).
const BuiltinStaticFace* BuiltinStaticFaces(size_t& count);

//The static-face qualifier singleton: the arm binds the outer
//identifier of the member call to this decl (pointer identity is the
//codegen marker), keeping the tree fully resolved. MINTING version
//(resolver only). Deliberately outside IsBuiltinClassName: bare
//`Future` stays rejected in every type position (a generic type needs
//arguments) — the singleton exists solely as the call qualifier.
//Per-build singletons: they live in ModuleRegistry's
//SyntheticBuiltinRegistry, so both functions take the registry.
class ModuleRegistry;
SnClassDecl* BuiltinStaticFaceClassDecl(ModuleRegistry& reg,
	const std::string& typeName);
//Non-minting probe (codegen): nullptr until the resolver minted one for
//this type name.
SnClassDecl* FindBuiltinStaticFaceClassDecl(const ModuleRegistry& reg,
	const std::string& typeName);

} //namespace nlang
