/*---
    ExprResolverBuiltinClasses.cpp — 内建类单例机器（ByteStream/FileStream/Object/Exception 族）
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
//Phase 9d: built-in Exception hierarchy. Each subclass declares Exception
//as its super so IsExceptionSubclass's chain walk succeeds. These are
//type-system stand-ins — actual ctor/fields live in the CompiledClass
//emitted by VmBackend::RegisterBuiltinClasses.
static SnClassDecl* s_pExceptionClass = nullptr;
static SnClassDecl* s_pNullPtrExcClass = nullptr;
static SnClassDecl* s_pDivZeroExcClass = nullptr;
static SnClassDecl* s_pOobExcClass = nullptr;
static SnClassDecl* s_pAssertExcClass = nullptr;
static SnClassDecl* s_pIoExcClass = nullptr;  //Phase 11: IOException

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
	//Phase 9d: force-create Exception singleton first so subclass chain
	//walk has a target even if the subclass is requested first.
	if (IsBuiltinExceptionClassName(name) && !s_pExceptionClass) {
		auto* pName = new std::string("Exception");
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation fallback;
		s_pExceptionClass = new SnClassDecl(pName, nullptr, pMembers,
			DeclLocation(pLoc, fallback));
		s_pExceptionClass->SetBuiltinClass();
	}
	SnClassDecl*& rpRef = (name == "ByteStream") ? s_pByteStreamClass
		: (name == "FileStream") ? s_pFileStreamClass
		: (name == "Object") ? s_pObjectClass
		: (name == "Exception") ? s_pExceptionClass
		: (name == "NullPointerException") ? s_pNullPtrExcClass
		: (name == "DivByZeroException") ? s_pDivZeroExcClass
		: (name == "IndexOutOfBoundsException") ? s_pOobExcClass
		: (name == "AssertionException") ? s_pAssertExcClass
		: (name == "IOException") ? s_pIoExcClass
		: s_pObjectClass;  //unreachable: IsBuiltinClassName gate above
	if (!rpRef)
	{
		auto* pName = new std::string(name);
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation fallback;
		rpRef = new SnClassDecl(pName, nullptr, pMembers,
			DeclLocation(pLoc, fallback));
		rpRef->SetBuiltinClass();
		//Phase 9d: subclasses point at Exception singleton for chain walk.
		if (name != "Exception" && IsBuiltinExceptionClassName(name))
			rpRef->SuperClass(s_pExceptionClass);
		rpRef->SetBuiltinClass();
	}
	return rpRef;
}

} //namespace nlang
