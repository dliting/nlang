/*--- ExprResolverGenerics.cpp — 内建泛型类实例化机器（List/Dict/Func 合成声明缓存）。
    从 ExprResolverTypes.cpp 抽取（合并期可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{


//Phase 8e-3: Built-in generic class instantiation cache.
//Key: (base class name, resolved type-arg SnField* pointers).
//Value: synthetic SnClassDecl representing this instantiation. The same
//key MUST always return the same SnClassDecl* — pointer identity is
//load-bearing for CalcTypeDistance and for VmBackend's class index lookup.
//
//At runtime, all instantiations share a single CompiledClass named "List"
//via VmBackend's name-prefix strip. The synthetic SnClassDecl is a
//compile-time artifact only.
struct GenericInstKey {
	std::string baseName;
	std::vector<SnField*> typeArgs;
	//Phase 13: out markers live on the type-arg expression nodes (NF_Out),
	//not on the canonical fields, so the key needs a parallel flag vector —
	//without it Func<void,int> and Func<void,out int> collapse into one
	//declaration and the first instantiation silently wins.
	//0.7.3 B: array-ness needs no flag vector anymore — an array-typed
	//argument IS the interned SnArrayTypeToken in typeArgs, so pointer
	//identity keeps List<int> and List<int[]> distinct keys.
	std::vector<uint8> outFlags;
	bool operator<(const GenericInstKey& rhs) const {
		if (baseName != rhs.baseName) return baseName < rhs.baseName;
		if (typeArgs.size() != rhs.typeArgs.size())
			return typeArgs.size() < rhs.typeArgs.size();
		if (outFlags != rhs.outFlags) return outFlags < rhs.outFlags;
		for (size_t i = 0; i < typeArgs.size(); ++i) {
			if (typeArgs[i] != rhs.typeArgs[i])
				return typeArgs[i] < rhs.typeArgs[i];
		}
		return false;
	}
};
static std::map<GenericInstKey, SnClassDecl*> s_genericInstances;

//Side table: synthetic class → its type arguments. Used by Access(SnMemberExpr&)
//to compute method return types (e.g., List<int>.Get() returns int = typeArgs[0]).
//Also mirrored on SnClassDecl::GenericTypeArgs() for VmBackend codegen.
static std::map<SnClassDecl*, std::vector<SnField*>> s_genericTypeArgs;

//Phase 13: parallel out-flag side table (same key discipline as
//s_genericTypeArgs). Func's delegate-binding channel reads the signature
//(return type + out-marked params) from these two tables.
static std::map<SnClassDecl*, std::vector<uint8>> s_genericOutFlags;

//Phase 13: out-flag side-table accessor (same key discipline as
//GetGenericTypeArgs; map operator[] default-inserts on miss, matching
//the historical direct map access at the Func bind sites).
const std::vector<uint8>& GetGenericOutFlags(SnClassDecl* pClass)
{
	return s_genericOutFlags[pClass];
}

//Lookup type arguments for a synthetic generic class. Returns empty vector
//if not a generic instantiation.
std::vector<SnField*> GetGenericTypeArgs(SnClassDecl* pClass)
{
	if (pClass && pClass->IsGenericInstantiation())
		return pClass->GenericTypeArgs();
	auto it = s_genericTypeArgs.find(pClass);
	if (it != s_genericTypeArgs.end())
		return it->second;
	return {};
}

//Returns true if name is a recognized built-in generic class.
//Phase 8e-3: "List" (arity 1). Phase 8e-4: "Dict" (arity 2).
//Phase 13: "Func" (variadic, at least the return type).
bool IsBuiltinGenericClassName(const std::string& name)
{
	return name == "List" || name == "Dict" || name == "Func";
}

//Returns true if class decl is a synthetic generic instantiation
//(e.g., List<int>). Used to dispatch member calls in Access(SnMemberExpr&).
bool IsGenericClassDecl(SnClassDecl* pClass)
{
	return pClass && pClass->IsGenericInstantiation();
}

//Mints (or fetches) a synthetic SnClassDecl for the given generic
//instantiation. Phase 8e-3: List<T> (arity 1). Phase 8e-4: Dict<K,V> (arity 2).
//Phase 13: Func<R, P...> (variadic, >= 1). outFlags are normalized here
//so callers that infer instantiations (List element inference) can pass
//an empty vector while declaration-path keys stay distinct.
SnClassDecl* GetGenericClassDecl(const std::string& baseName,
	const std::vector<SnField*>& typeArgs, const std::vector<uint8>& outFlags,
	const ISourceLocation* pLoc)
{
	std::vector<uint8> normOutFlags(outFlags);
	normOutFlags.resize(typeArgs.size(), 0);
	GenericInstKey key{baseName, typeArgs, normOutFlags};
	auto it = s_genericInstances.find(key);
	if (it != s_genericInstances.end())
		return it->second;

	//Built-in generic + arity check. List/Dict have fixed arity; Func is
	//variadic: first argument is the return type, the rest are params.
	bool isFunc = baseName == "Func";
	size_t expectedArity = (baseName == "Dict") ? 2 : 1;
	size_t minArity = isFunc ? 1 : expectedArity;
	size_t maxArity = isFunc ? static_cast<size_t>(-1) : expectedArity;
	if (!IsBuiltinGenericClassName(baseName)
		|| typeArgs.size() < minArity || typeArgs.size() > maxArity)
		return nullptr;

	//Build display name e.g. "List<int>", "Dict<string, int>". Array-typed
	//arguments ARE interned tokens; their Name() is empty by design, so
	//render them via ToString ("Int32[]") — without this the split
	//instantiations would be indistinguishable in diagnostics.
	std::string instName = baseName + "<";
	for (size_t i = 0; i < typeArgs.size(); ++i) {
		if (i) instName += ", ";
		if (typeArgs[i]->Kind() == NK_ArrayTypeToken)
			instName += typeArgs[i]->ToString();
		else
			instName += typeArgs[i]->Name();
	}
	instName += ">";

	auto* pName = new std::string(instName);
	auto* pMembers = new PtrList<SnField>();
	ScriptLocation fallback;
	auto* pClass = new SnClassDecl(pName, nullptr, pMembers,
		DeclLocation(pLoc, fallback));
	pClass->SetBuiltinClass();
	pClass->SetGenericInstantiation();
	pClass->SetGenericTypeArgs(typeArgs);
	pClass->SetBaseName(baseName);
	s_genericInstances[key] = pClass;
	//Side table for member-call return-type lookup (List<int>.Get() → int).
	s_genericTypeArgs[pClass] = typeArgs;
	s_genericOutFlags[pClass] = normOutFlags;
	return pClass;
}


} //namespace nlang
