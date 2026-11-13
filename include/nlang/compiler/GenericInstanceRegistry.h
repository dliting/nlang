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

//0.13.1b: declaration-time cap on a generic declaration's type
//parameters. Ordinals are 0-based and travel in one byte on two
//channels — the frame's boundary-box plan and the typeName/T-equality
//byte, whose top value (0xFF) is the no-ordinal marker
//(SnTypeNameExpr::kNoTypeParamOrdinal). The 256th parameter's ordinal
//would collide with that sentinel; the cap keeps every ordinal ≤ 254.
constexpr size_t kMaxGenericTypeParams = 255;

class SnClassDecl;
class SnField;
class SnInterfaceDecl;
class SyntaxNode;

class GenericInstanceRegistry
{
public:
	~GenericInstanceRegistry();

	//The template's placeholder for the type parameter `name` at
	//parameter position `position` (interned on first request; one node
	//per (template, parameter) pair). The template node is a generic
	//CLASS declaration or — 0.13.1a — a generic free FUNCTION
	//declaration; both mint the same opaque placeholder node.
	SnField* PlaceholderFor(SyntaxNode& tmpl, const std::string& name,
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

	//The minted instantiation a clone member belongs to, recorded by the
	//same fill-time pair point as AdoptMemberClone. The backend's static-
	//call arm reads it for the frame descriptor cell (a clone static's
	//cell is the instantiation's descriptor row); null for non-clones.
	SnClassDecl* InstantiationOfClone(const SnField* pMember) const;

	//0.13.1c Task 8: the interface twin of the clone-to-instantiation
	//pair. The backend's static-call arm consults it when a clone
	//static's frame descriptor names a generic INTERFACE instantiation
	//(the class channel answers class clones; the two never alias —
	//each adopt names its own kind).
	SnInterfaceDecl* InstantiationOfInterfaceClone(
		const SnField* pMember) const;
	void AdoptInterfaceCloneInstantiation(SnField* pClone,
		const SnInterfaceDecl* pInstance);

	//Record the clone-to-instantiation pair for one instantiation member
	//(called by the member fill right after the copy joins the shell).
	void AdoptCloneInstantiation(SnField* pClone,
		const SnClassDecl* pInstance);

	//The minted instance for (template, type arguments), or null when
	//this combination has not been minted yet.
	SnClassDecl* FindInstance(const SnClassDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs) const;

	//Iterate every declared instantiation, in mint order. Compile-time
	//surfaces (diagnostics, tests) enumerate through this; the runtime
	//records never do — they live on the template rows.
	void ForEachInstance(
		const std::function<void(SnClassDecl&)>& visitor) const;

	//0.13.1c: the interface channel, parallel to the class maps. The
	//class paths keep their exact types and consumers; GetDeclared-
	//GenericInterfaceDecl is the only writer, via the same two-phase
	//publish-then-fill contract (AdoptInterfaceInstance before the
	//recursive fill window).
	SnInterfaceDecl* FindInterfaceInstance(
		const SnInterfaceDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs) const;
	void AdoptInterfaceInstance(SnInterfaceDecl* pInstance,
		const SnInterfaceDecl* pTemplate,
		const std::vector<const SnField*>& typeArgs);
	//The interface template a minted interface instantiation was minted
	//from; null when the node is not one.
	SnInterfaceDecl* InstanceOfInterface(
		const SnInterfaceDecl* pInstance) const;

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
	//state. The key is node identity: the class and interface templates
	//dedup in one set (implicit upcast — no cast at call sites).
	bool MarkDepthLimitReported(const SyntaxNode* pTemplate)
	{
		return m_depthLimitReportedTmpls.insert(pTemplate).second;
	}

private:
	//Pointer-identity key: the template node plus the interned type-
	//argument fields (interned tokens keep identity per element type).
	//Templated on the template-node kind (0.13.1c): the class channel
	//keys on SnClassDecl, the interface channel on SnInterfaceDecl —
	//one comparison shape, no cross-kind pointer punning.
	template<typename T>
	struct InstanceKey
	{
		const T* pTemplate;
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
		const SyntaxNode* pTemplate;
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
	std::map<InstanceKey<SnClassDecl>, SnClassDecl*> m_instances;
	std::map<const SnClassDecl*, const SnClassDecl*> m_instanceTemplate;
	//0.13.1c interface channel, parallel to the class maps (kept exact
	//so the class paths and their consumers stay untouched).
	std::map<InstanceKey<SnInterfaceDecl>, SnInterfaceDecl*> m_ifaceInstances;
	std::map<const SnInterfaceDecl*, const SnInterfaceDecl*>
		m_ifaceInstanceTemplate;
	std::map<const SnField*, const SnField*> m_memberRecordOwner;
	std::map<const SnField*, const SnClassDecl*> m_memberInstantiation;
	//0.13.1c Task 8: the interface twin of m_memberInstantiation (the
	//minimal-channel ruling — class paths keep their exact types).
	std::map<const SnField*, const SnInterfaceDecl*> m_ifaceCloneInstantiation;
	int m_iGenericMintDepth = 0;
	std::set<const SyntaxNode*> m_depthLimitReportedTmpls;
};

} //namespace nlang

#endif //NLANG_COMPILER_GENERIC_INSTANCE_REGISTRY_H
