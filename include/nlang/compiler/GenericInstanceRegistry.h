/*---
GenericInstanceRegistry.h - declared-generic placeholder and
instantiation registry, scoped to one build.

Compile-time machinery for `class Name<T, ...>` declarations: the
template resolves its member signatures against interned placeholder
types (one per type parameter), and each `Name<Arg...>` use mints a
synthetic instantiation whose members are signature deep-copies with
placeholders substituted. All artifacts are compile-time only: the
instantiations never enter the syntax tree; the VM backend reads them
through the accessors below (per-unit registration and method-table
population).

The registry is owned by ModuleRegistry — one instance per BUILD. A
process-global cache would be keyed by per-build values (raw tree
pointers, unit indices that restart at 0), so a second in-process
build would inherit the first build's entries: the unit-index
collision walks stale instances and corrupts the build (observed as a
silent exception in the in-process test harness). The registry also
owns every node it mints (placeholders and instantiations) and frees
them when the build ends.
---*/
#ifndef NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
#define NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nlang
{

class SnClassDecl;
class SnField;

class GenericInstanceRegistry
{
public:
	~GenericInstanceRegistry();

	//The template's placeholder for the type parameter `name` at
	//parameter position `position` (interned on first request; one node
	//per (template, parameter) pair).
	SnField* PlaceholderFor(SnClassDecl& tmpl, const std::string& name,
		size_t position);

	//Is this field one of the interned type-parameter placeholders?
	bool IsPlaceholder(const SnField* pField) const;

	//True when any type argument (transitively — a nested
	//instantiation's arguments count) is a template placeholder. Such
	//an instance was minted while resolving a template's OWN signature
	//(Dict.keys(): List<K>): it exists only as a compile-time type
	//surface — its substituted members still carry placeholder leaves,
	//and no runtime path binds or mints it. Concrete-argument
	//instances minted at use sites are the emittable form; emission
	//walks skip placeholder forms (ModuleRegistry::
	//ForEachOwnGenericInstance).
	bool HasPlaceholderArg(const SnClassDecl& inst) const;

	//The parameter position a placeholder stands for (an index into the
	//template's TypeParamNames()). Precondition: IsPlaceholder.
	size_t PlaceholderIndex(const SnField* pPlaceholder) const;

	//The template a declared instantiation was minted from; null when
	//the field is not a declared-generic instantiation.
	SnClassDecl* InstanceOf(const SnClassDecl* pInstance) const;

	//Record a freshly minted instantiation. GetDeclaredGenericClassDecl
	//is the only minting site; this call publishes the instance for the
	//registration walks below.
	void AdoptInstance(SnClassDecl* pInstance, const SnClassDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs, uint32_t ownerUnit);

	//The minted instance for (template, type arguments), or null when
	//this combination has not been minted yet.
	SnClassDecl* FindInstance(const SnClassDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs) const;

	//Iterate the declared instantiations minted while resolving
	//`ownerUnit`, in mint order. The VM backend calls this per unit
	//image to register the instantiation classes and their member
	//records.
	void ForEachInstance(uint32_t ownerUnit,
		const std::function<void(SnClassDecl&)>& visitor) const;

private:
	//Pointer-identity key: the template node plus the interned type-
	//argument fields (interned tokens keep identity per element type).
	struct InstanceKey
	{
		const SnClassDecl* pTemplate;
		std::vector<const SnField*> typeArgs;
		bool operator<(const InstanceKey& rhs) const
		{
			if (pTemplate != rhs.pTemplate)
				return pTemplate < rhs.pTemplate;
			if (typeArgs.size() != rhs.typeArgs.size())
				return typeArgs.size() < rhs.typeArgs.size();
			for (size_t i = 0; i < typeArgs.size(); ++i)
				if (typeArgs[i] != rhs.typeArgs[i])
					return typeArgs[i] < rhs.typeArgs[i];
			return false;
		}
	};
	struct PlaceholderKey
	{
		const SnClassDecl* pTemplate;
		std::string name;
		bool operator<(const PlaceholderKey& rhs) const
		{
			if (pTemplate != rhs.pTemplate)
				return pTemplate < rhs.pTemplate;
			return name < rhs.name;
		}
	};

	std::map<PlaceholderKey, SnField*> m_placeholders;
	std::map<const SnField*, size_t> m_placeholderIndex;
	std::map<InstanceKey, SnClassDecl*> m_instances;
	std::map<const SnClassDecl*, const SnClassDecl*> m_instanceTemplate;
	std::map<uint32_t, std::vector<SnClassDecl*>> m_instancesByUnit;
};

} //namespace nlang

#endif //NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
