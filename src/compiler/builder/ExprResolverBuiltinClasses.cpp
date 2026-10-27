/*---
    ExprResolverBuiltinClasses.cpp — 内建类单例机器（ByteStream/FileStream）
    与预置包类声明查找（FindPreludeClassDecl/EnsureImplicitObjectSuperclass，
    2026-10-27 尺寸守卫触发的拆分）。Object 随内建类真源化（Task 5）声明化
    退役：预置包声明经作用域链解析，无单例。
---*/
#include "ExprResolver.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"

namespace nlang
{

//Canonical SnClassDecl instances for built-in classes. These must be
//singletons so that CalcTypeDistance's pointer identity check works
//across all resolution sites (type names, new expressions, member calls).
static SnClassDecl* s_pByteStreamClass = nullptr;
static SnClassDecl* s_pFileStreamClass = nullptr;
//Channel needs no stand-in: it exists only as Channel<T>, a generic
//instantiation minted by GetGenericClassDecl. Mutex/Semaphore, Thread
//and Object lost their stand-ins to the builtin-decls migration —
//declared in stdlib/builtins.n now.

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
		: s_pFileStreamClass;
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

SnClassDecl *FindPreludeClassDecl(const ModuleRegistry &reg,
	const std::string &name)
{
	SnNamespace *pRoot = TheAST().Root();
	if (!pRoot)
		return nullptr;
	auto range = pRoot->Members().NameDict().equal_range(name);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField &member = *iField->second;
		if (member.Kind() != NK_ClassDecl)
			continue;
		const uint32_t owner = reg.OwnerOf(member);
		if (owner != ModuleRegistry::NO_OWNER && reg.IsPreludeModule(owner))
			return static_cast<SnClassDecl *>(&member);
	}
	return nullptr;
}

void EnsureImplicitObjectSuperclass(SnClassDecl &sn,
	const ModuleRegistry &reg)
{
	if (sn.SuperName() || sn.SuperClass())
		return;
	SnClassDecl *pObject = FindPreludeClassDecl(reg, "Object");
	if (!pObject || pObject == &sn)
		return;
	sn.SuperClass(pObject);
}

} //namespace nlang
