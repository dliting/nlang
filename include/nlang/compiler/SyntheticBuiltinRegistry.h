/*---
    SyntheticBuiltinRegistry.h - synthetic builtin declarations minted
    on demand, scoped to one build.

    Compile-time cache for the resolver's on-demand synthesis: builtin
    generic instantiations (List<int>, Dict<string,int>,
    func<void,out int>, ... keyed by base name + interned type-argument
    pointers + out flags), the non-generic builtin class singletons
    (ByteStream, FileStream) and the static-face qualifier singletons
    (the `Future` of Future.selectAny). Pointer identity of the minted
    nodes is load-bearing (CalcTypeDistance, the backend's codegen
    markers), so one key must always return the same node — within a
    build.

    Owned by ModuleRegistry — one instance per BUILD, for the same
    reason GenericInstanceRegistry is: a process-global cache would be
    keyed by raw tree pointers, and once the build's syntax tree is
    cleared a later allocation reusing a freed address would silently
    inherit the stale entry (whose type arguments dangle into the
    cleared tree). The registry owns every node it mints and frees them
    when the build ends. Storage and ownership only — the minting logic
    (name checks, arity checks, node construction) stays at the
    resolver's free functions, the same split GenericInstanceRegistry
    keeps with GetDeclaredGenericClassDecl.
---*/
#ifndef NLANG_COMPILER_SYNTHETIC_BUILTIN_REGISTRY_H
#define NLANG_COMPILER_SYNTHETIC_BUILTIN_REGISTRY_H

#include <nlang/runtime/TypeDef.h>
#include <map>
#include <string>
#include <vector>

namespace nlang
{

class SnClassDecl;
class SnField;

class SyntheticBuiltinRegistry
{
public:
	~SyntheticBuiltinRegistry();

	//--- builtin generic instantiations ---------------------------------
	//Key: (base class name, resolved type-arg SnField pointers, out
	//flags). Interned type-arg tokens keep pointer identity per element
	//type within one build, so the pointers are the cache discipline;
	//the out flags ride a parallel vector because out markers live on
	//type-arg EXPRESSIONS (NF_Out), not on the canonical fields.
	struct GenericInstKey
	{
		std::string baseName;
		std::vector<SnField*> typeArgs;
		std::vector<uint8> outFlags;
		bool operator<(const GenericInstKey& rhs) const
		{
			if (baseName != rhs.baseName)
				return baseName < rhs.baseName;
			if (typeArgs.size() != rhs.typeArgs.size())
				return typeArgs.size() < rhs.typeArgs.size();
			if (outFlags != rhs.outFlags)
				return outFlags < rhs.outFlags;
			for (size_t i = 0; i < typeArgs.size(); ++i)
				if (typeArgs[i] != rhs.typeArgs[i])
					return typeArgs[i] < rhs.typeArgs[i];
			return false;
		}
	};
	//The minted instantiation for a key, or null before the first mint.
	SnClassDecl* FindGeneric(const GenericInstKey& key) const;
	//Publish a freshly minted instantiation (the node itself carries its
	//type arguments and out flags — GenericTypeArgs/GenericOutFlags —
	//so context-free readers never need this registry).
	void AdoptGeneric(const GenericInstKey& key, SnClassDecl* pDecl);

	//--- non-generic builtin singletons (ByteStream/FileStream) -------
	//Null until minted; the minting free function owns the name check
	//and node construction.
	SnClassDecl* FindBuiltinClass(const std::string& name) const;
	void AdoptBuiltinClass(const std::string& name, SnClassDecl* pDecl);

	//--- static-face qualifier singletons ------------------------------
	//Same shape: null until the resolver mints the call qualifier.
	SnClassDecl* FindFaceClass(const std::string& typeName) const;
	void AdoptFaceClass(const std::string& typeName, SnClassDecl* pDecl);

	//Diagnostics/tests: how many nodes this registry owns. A FRESH
	//registry mints 0 — the second-build seam (ModuleRegistry::Reset)
	//pins exactly that: after a reset, the next build re-mints the same
	//set (same count), while a surviving registry accumulates (double).
	size_t MintedCount() const { return m_minted.size(); }

private:
	//Every minted node goes on the ownership list; the destructor frees
	//them (the nodes are detached from any tree, so nothing else does).
	void Own(SnClassDecl* pDecl);

	std::map<GenericInstKey, SnClassDecl*> m_genericInstances;
	std::map<std::string, SnClassDecl*> m_builtinClasses;
	std::map<std::string, SnClassDecl*> m_faceClasses;
	std::vector<SnClassDecl*> m_minted;
};

} //namespace nlang

#endif //NLANG_COMPILER_SYNTHETIC_BUILTIN_REGISTRY_H
