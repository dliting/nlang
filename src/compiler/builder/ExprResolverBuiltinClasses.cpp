/*---
    ExprResolverBuiltinClasses.cpp — 内建类单例机器（ByteStream/FileStream/Object/Thread）
    从 ExprResolverTypes.cpp 抽取（2026-09-27 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuiltinNames.h"

namespace nlang
{

//Canonical SnClassDecl instances for built-in classes. These must be
//singletons so that CalcTypeDistance's pointer identity check works
//across all resolution sites (type names, new expressions, member calls).
static SnClassDecl* s_pByteStreamClass = nullptr;
static SnClassDecl* s_pFileStreamClass = nullptr;
static SnClassDecl* s_pObjectClass = nullptr;  //Phase 8e-1: implicit Object base class
//K1: the Thread type-system stand-in (the real method/field surface of
//the concurrency builtins used to live in VmBackend's s_BuiltinClassDecls
//rows). Channel needs no stand-in: it exists only as Channel<T>, a
//generic instantiation minted by GetGenericClassDecl. Mutex/Semaphore
//lost their stand-ins to the builtin-decls migration — declared in
//stdlib/builtins.n now.
static SnClassDecl* s_pThreadClass = nullptr;

//On-demand minted decls (built-in classes, generic instantiations) take
//the caller's location when one exists. That location is polymorphic —
//a parser-minted ScriptLocation for source nodes, the imported-module
//location for v1.12 stub nodes — so it must flow through the
//ISourceLocation interface (SyntaxNode clones whatever it receives).
//Punning it to ScriptLocation reads past the end of the smaller
//imported-location object.
const ISourceLocation& DeclLocation(const ISourceLocation* pLoc,
	ScriptLocation& fallback)
{
	return pLoc ? *pLoc : fallback;
}

//Precondition: name passes IsBuiltinClassName. Returns nullptr for any
//other name (defensive — callers skip resolution and the identifier
//surfaces as a normal unresolved-name error).
SnClassDecl* GetBuiltinClassDecl(const std::string& name,
	const ISourceLocation* pLoc)
{
	if (!IsBuiltinClassName(name))
		return nullptr;
	SnClassDecl*& rpRef = (name == "ByteStream") ? s_pByteStreamClass
		: (name == "FileStream") ? s_pFileStreamClass
		: (name == "Object") ? s_pObjectClass
		: s_pThreadClass;
	if (!rpRef)
	{
		auto* pName = new std::string(name);
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation fallback;
		rpRef = new SnClassDecl(pName, nullptr, pMembers,
			DeclLocation(pLoc, fallback));
		rpRef->SetBuiltinClass();
	}
	return rpRef;
}

} //namespace nlang
