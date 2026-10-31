/*---
GenericInstanceRegistry.h - declared-generic placeholder and
instantiation registry, scoped to one build.

Compile-time machinery for `class Name<T, ...>` declarations: the
template resolves its member signatures against interned placeholder
types (one per type parameter), and each `Name<Arg...>` use mints a
synthetic instantiation whose members are signature deep-copies with
placeholders substituted. All artifacts are compile-time only: the
instantiations never enter the syntax tree. The dictionary model keeps
one implementation per template — the compiled records stay with the
template row in its owning unit, and the backend's slot throats resolve
instantiation references to those records (same unit: table hit;
another unit: the standard import-slot path).

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
#include <set>
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

	//The parameter position a placeholder stands for (an index into the
	//template's TypeParamNames()). Precondition: IsPlaceholder.
	size_t PlaceholderIndex(const SnField* pPlaceholder) const;

	//The template a declared instantiation was minted from; null when
	//the field is not a declared-generic instantiation.
	SnClassDecl* InstanceOf(const SnClassDecl* pInstance) const;

	//v0.12.3 dictionary model: the record-owning TEMPLATE member for an
	//instantiation's signature copy, recorded by the member fill at
	//clone time (the pair is known exactly there — a later mutation of
	//either member list cannot drift the mapping). Returns null for
	//anything that is not an instantiation member (plain classes own
	//their records). One authority for the clone-to-owner mapping: the
	//resolver keeps clones bound (every type read wants the substituted
	//surface) and the backend's slot throats resolve to the owner here
	//(every record read wants the template row).
	SnField* RecordOwnerOfMember(const SnField* pMember) const;

	//Record a freshly minted instantiation. GetDeclaredGenericClassDecl
	//is the only minting site; this call publishes the instance in the
	//cache and links it to its template.
	void AdoptInstance(SnClassDecl* pInstance, const SnClassDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs);

	//Record the clone-to-record-owner pair for one instantiation member
	//(called by the member fill right after the copy joins the shell).
	void AdoptMemberClone(SnField* pClone, const SnField* pSource);

	//The minted instance for (template, type arguments), or null when
	//this combination has not been minted yet.
	SnClassDecl* FindInstance(const SnClassDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs) const;

	//Iterate every declared instantiation, in mint order. Compile-time
	//surfaces (diagnostics, tests) enumerate through this; the runtime
	//records never do — they live on the template rows.
	void ForEachInstance(
		const std::function<void(SnClassDecl&)>& visitor) const;

	//Concurrent-mint depth, the live instantiation chain: nested mints
	//fire synchronously inside BOTH phases of the mint window — the
	//template-completion preamble resolves member types and the member
	//cloning substitutes them — each nested mint through a freshly
	//constructed resolver, so the chain depth is registry state rather
	//than resolver state. Enter/Leave pair is called by the resolver's
	//RAII claim around the window; the depth guard at the mint entry
	//reads the value.
	int GenericMintDepth() const { return m_iGenericMintDepth; }
	void EnterMintDepth() { ++m_iGenericMintDepth; }
	void LeaveMintDepth() { --m_iGenericMintDepth; }

	//Depth-limit diagnostic bookkeeping: true on the FIRST guard trip
	//for this template — the caller logs the depth diagnostic then;
	//later trips of the same template family stay silent. One defect,
	//one diagnostic: the unwinding resolution arms retry the chain
	//(measured at 131071 guard trips for one six-line program), and
	//every retry of an already-reported family repeats the same
	//message. Presentation-only dedup — never gates a mint. Per-build
	//state.
	bool MarkDepthLimitReported(const SnClassDecl* pTemplate)
	{
		return m_depthLimitReportedTmpls.insert(pTemplate).second;
	}

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
	std::map<const SnField*, const SnField*> m_memberRecordOwner;
	int m_iGenericMintDepth = 0;
	std::set<const SnClassDecl*> m_depthLimitReportedTmpls;
};

} //namespace nlang

#endif //NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
