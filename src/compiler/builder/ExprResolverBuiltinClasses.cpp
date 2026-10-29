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
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/vm/Intrinsics.h>
#include <map>

namespace nlang
{

//Canonical SnClassDecl instances for built-in classes. These must be
//singletons WITHIN A BUILD so that CalcTypeDistance's pointer identity
//check works across all resolution sites (type names, new expressions,
//member calls); the singletons live in the build's
//SyntheticBuiltinRegistry (a process-global would keep pointers alive
//past the tree they were minted against). Channel needs no stand-in:
//it exists only as Channel<T>, a generic instantiation minted by
//GetGenericClassDecl. Mutex/Semaphore, Thread and Object lost their
//stand-ins to the builtin-decls migration — declared in
//stdlib/builtins.n now.

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
SnClassDecl* GetBuiltinClassDecl(ModuleRegistry& reg,
	const std::string& name, const ISourceLocation* pLoc)
{
	if (!IsBuiltinClassName(name))
		return nullptr;
	auto& synthetics = reg.SyntheticBuiltins();
	if (SnClassDecl* pExisting = synthetics.FindBuiltinClass(name))
		return pExisting;
	auto* pName = new std::string(name);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(pLoc, fallback));
	pClass->SetBuiltinClass();
	synthetics.AdoptBuiltinClass(name, pClass);
	return pClass;
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

//Face-holder classes (v0.12.2, spec D10): String/Array declare the
//method faces of the string keyword and array types. They have no
//instances and no virtual dispatch shapes, so they stay out of the
//class hierarchy — no implicit Object base. This also keeps their
//equals/getHashCode/toString out of the virtual-contract bijection:
//PropagateVirtualOverrides matches by name + param COUNT, so a based
//String would collide with Object.equals(Object) (the
//ovsig_equals_mismatch probe pins that rejection for user classes).
//The bare-name predicate is inline in BuiltinNames.h (shared with the
//mirror name-yield pass).
bool IsPreludeFaceClass(const ModuleRegistry &reg, const SnClassDecl &sn)
{
	return IsPreludeFaceClassName(sn.Name())
		&& &sn == FindPreludeClassDecl(reg, sn.Name());
}

void EnsureImplicitObjectSuperclass(SnClassDecl &sn,
	const ModuleRegistry &reg)
{
	if (sn.SuperName() || sn.SuperClass())
		return;
	//Face classes stay rootless (spec D10) — identity check, so a user
	//class named String still gets Object like any other class.
	if (IsPreludeFaceClass(reg, sn))
		return;
	SnClassDecl *pObject = FindPreludeClassDecl(reg, "Object");
	if (!pObject || pObject == &sn)
		return;
	sn.SuperClass(pObject);
}

//--- static faces on builtin generic type names --------------------------------

//The shared resolver/backend face table (Future.selectAny today). The
//resolver's arm gates and arity-checks from these rows; the backend's
//emission reads the intrinsic id from the SAME row and never evaluates
//the qualifier — one row, one identity, no duplicated dispatch. The
//binding table (BindingTable.cpp) derives its static-face binding rows
//from this table too, so a second face lands in exactly one
//hand-maintained place.
static const BuiltinStaticFace s_Faces[] = {
	{kBuiltinFutureTypeName, "selectAny", INTR_Future_SelectAny, 1},
};

//Every face row (the derivation input for the binding table's
//static-face rows).
const BuiltinStaticFace* BuiltinStaticFaces(size_t& count)
{
	count = sizeof(s_Faces) / sizeof(s_Faces[0]);
	return s_Faces;
}

const BuiltinStaticFace* FindBuiltinStaticFace(const std::string& typeName,
	const std::string& methodName)
{
	for (const auto& face : s_Faces)
		if (typeName == face.typeName && methodName == face.methodName)
			return &face;
	return nullptr;
}

//The face qualifier singletons (one per face type name), minted like
//GetBuiltinClassDecl's singletons above — SetBuiltinClass marks them
//synthetic — but deliberately OUTSIDE IsBuiltinClassName: bare `Future`
//must stay rejected in every type position (a generic type needs
//arguments). The decl exists solely as the outer identifier's binding
//so the tree resolves fully; pointer identity against it is the
//codegen marker (the backend never emits the qualifier as a value).
//Per-build singletons (the build's SyntheticBuiltinRegistry) — the
//pointer identity they exist for only means anything within one build.
SnClassDecl* BuiltinStaticFaceClassDecl(ModuleRegistry& reg,
	const std::string& typeName)
{
	auto& synthetics = reg.SyntheticBuiltins();
	if (SnClassDecl* pExisting = synthetics.FindFaceClass(typeName))
		return pExisting;
	auto* pName = new std::string(typeName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(nullptr, fallback));
	pClass->SetBuiltinClass();
	synthetics.AdoptFaceClass(typeName, pClass);
	return pClass;
}

//Non-minting probe: null until the resolver minted this type name's
//qualifier (codegen's pointer-identity check consults it).
SnClassDecl* FindBuiltinStaticFaceClassDecl(const ModuleRegistry& reg,
	const std::string& typeName)
{
	return reg.SyntheticBuiltins().FindFaceClass(typeName);
}
} //namespace nlang
