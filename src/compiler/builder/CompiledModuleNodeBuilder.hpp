#pragma once
#include "CompiledModuleStubMembers.hpp"
#include "CompiledModuleDefaultSupport.hpp"
#include "CompiledModuleStubSupport.hpp"
#include "SyntaxTree.h"
#include "SnMisc.h"
#include "SnData.h"
#include "SnExpressions.h"
#include "SnStatements.h"
#include "BuildEnvironment.h"
#include "nlang/vm/CompiledModule.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/RnTypes.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

namespace nlang
{

//Builds Sn* AST stubs directly from a CompiledModule (the in-memory form of
//a .ncu file), bypassing the legacy RnFunction / RuntimeNode pipeline.
//
//Stubs are intentionally minimal: they carry enough structural information
//(name, param count, member names) for VmBackend to skip codegen on them
//(via NF_Imported + missing Body) and for the resolver to recognize the
//symbol as an imported declaration. Formal and return types come from the
//v1.12 type descriptors: this builder synthesizes plain syntactic type
//expressions (the same shapes the parser emits for hand-written source),
//so the consumer's normal ResolveDataTypes pass binds and interns them
//through the standard channels — cross-module call-site type checking is
//real, not skipped.
//
//After BuildFromCompiledModule, the caller can query ImportedFunctions()
//for the minted stubs (registry owner tagging in ModuleBuilder::
//RegisterExternalStubs). Phase 5: table keys are package-qualified
//("<pkg>.<name>"), but an imported stub's AST NAME must be the leaf —
//the resolver looks stubs up by the caller's bare callee/type name,
//while the stub's owner tag (the external module) supplies the package.
//The qualified key stays in the CompiledModule tables, where the
//load-time linker resolves it from the peer images.
class CompiledModuleNodeBuilder
{
public:
	//Construct a builder for one source module. registry (optional)
	//enables the declared-prelude superclass link in LinkSuperclassStub.
	CompiledModuleNodeBuilder(SyntaxTree &tree,
		const std::string &moduleName,
		const ModuleRegistry *registry = nullptr) :
		m_Tree(tree), m_moduleName(moduleName), m_pRegistry(registry)
	{
	}

	//Walk cm.functions/classes/structs and append Sn* stubs to m_Tree.Root().
	//Throws std::runtime_error on policy violations (e.g. imported module
	//defining main() — see Layer 6).
	//
	//R10-1 dedup: every compiled .ncu contains built-in functions/classes
	//emitted by RegisterBuiltinClasses (ByteStream/Dict/List constructors,
	//their methods, etc.) because the serializer writes everything in
	//m_compiledModule. The consumer's AST root already has these same
	//built-ins injected by SyntaxTree::BuildFromRuntime before this method
	//runs. Adding root entries for them would trigger DuplicateFieldChecker
	//errors. A function whose name already exists in root stays as a
	//DETACHED stub (registered, never pushed into root — see
	//TakeDetachedStubs()); structs/classes are skipped entirely.
	void BuildFromCompiledModule(const CompiledModule &cm)
	{
		CheckNoMainFunction(cm);

		//Own entries only: a unit image's tables carry placeholder tail
		//slots after the own entries (one per import record, in order).
		//Placeholders target OTHER modules — stubbing them into this
		//consumer's tree would mis-attribute their ownership.
		const size_t ownFuncs =
			cm.functions.size() - cm.functionImports.size();
		const size_t ownStructs =
			cm.structs.size() - cm.structImports.size();
		const size_t ownClasses =
			cm.classes.size() - cm.classImports.size();
		const size_t ownEnums =
			cm.enumNames.size() - cm.enumImports.size();

		auto loc = CompiledModuleNodeLocation(cm.name);

		//Helper: is there already a top-level field with this name in root?
		//Built-ins are injected by BuildFromRuntime before we run.
		auto nameExistsInRoot = [this](const std::string &name) -> bool {
			for (auto &m : TheRoot().Members())
				if (m.Name() == name)
					return true;
			return false;
		};

		//Build a set of CompiledFunction indices that are actually class
		//methods or constructors (referenced by some CompiledClass's
		//methodIndices / constructorIndices, or by some CompiledStruct's
		//constructorIndices — 0.9.11 v2.6). These must NOT be promoted to
		//free-function stubs — they belong to their owning type and are
		//already represented (via class/struct stub members) on the
		//consumer side. Without this filter, List.add / Dict.set / a struct
		//ctor would be promoted to root as free functions, and the user's
		//own free `add` would then be wrongly deduped against the imported
		//List.add (R10-1 collision).
		std::unordered_set<uint32_t> methodOrCtorIndices;
		for (size_t ci = 0; ci < ownClasses; ++ci)
		{
			for (uint16_t mi : cm.classes[ci].methodIndices)
				methodOrCtorIndices.insert(mi);
			for (uint16_t ctorIdx : cm.classes[ci].constructorIndices)
				methodOrCtorIndices.insert(ctorIdx);
		}
		for (size_t si = 0; si < ownStructs; ++si)
			for (uint16_t ctorIdx : cm.structs[si].constructorIndices)
				methodOrCtorIndices.insert(ctorIdx);
		//0.9.10 D5: interface statics are likewise owned by their
		//interface stub, never promoted to free functions.
		for (const auto &iface : cm.interfaces)
			for (uint16_t si : iface.staticMethodIndices)
				methodOrCtorIndices.insert(si);
		//0.10.14: enum methods are likewise owned by their enum stub —
		//without this they were promoted to free stubs under the leaf
		//name (the double-mint the enum import corrects).
		for (size_t ei = 0; ei < ownEnums; ++ei)
			for (uint16_t mi : cm.enumMethodIndices[ei])
				methodOrCtorIndices.insert(mi);

		//Functions first so that subsequent class/struct stub members can
		//reference them by name if needed (resolver does name-based lookup
		//later, so physical order among siblings doesn't matter — but having
		//functions first matches the typical parser emission order).
		for (uint32_t i = 0; i < ownFuncs; ++i)
		{
			if (methodOrCtorIndices.count(i))
				continue;  //class method/ctor — owned by its class, not free
			//Free functions have no declared member access on the wire —
			//the stub mints public (top-level declarations are public).
			//0.10.14: the table index rides the stub — the namespace
			//branch of the cross-unit slot record carries it.
			SnFunction *stub = CreateFunctionStub(cm.functions[i], cm,
				loc, FA_Public, ProducerIdxClamp(i));
			if (nameExistsInRoot(LeafNameOfKey(cm.functions[i].name)))
			{
				//R10-1: no second root entry for an existing name — but
				//the stub stays registered (ImportedFunctions) so its
				//module's stub table remains complete: qualified calls
				//resolve through the stub table, never the shared root.
				//Ownership passes to the caller via TakeDetachedStubs().
				m_detachedStubs.emplace_back(stub);
				m_importedFuncs.push_back(stub);
				continue;
			}
			m_Tree.Root()->Members().push_back(stub);
			m_importedFuncs.push_back(stub);
		}

		//Structs.
		for (size_t si = 0; si < ownStructs; ++si)
		{
			if (nameExistsInRoot(LeafNameOfKey(cm.structs[si].name)))
				continue;  //R10-1
			SnStructDecl *stub = CreateStructStub(cm.structs[si], cm, loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
		}

		//Classes. 0.9.12 (v2.7): each stub rebuilds its full member
		//surface — own fields, methods (static/instance, access, virtual
		//flag) and constructors (CreateClassStub); the super links run
		//after the loop so a subclass stub can point at its peer stub
		//regardless of declaration order.
		std::vector<std::pair<SnClassDecl*, const CompiledClass*>> classStubs;
		std::unordered_map<std::string, SnClassDecl*> peerStubs;
		for (size_t ci = 0; ci < ownClasses; ++ci)
		{
			if (nameExistsInRoot(LeafNameOfKey(cm.classes[ci].name)))
				continue;  //R10-1
			SnClassDecl *stub = CreateClassStub(cm.classes[ci], cm, loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
			classStubs.emplace_back(stub, &cm.classes[ci]);
			peerStubs.emplace(LeafNameOfKey(cm.classes[ci].name), stub);
		}
		for (auto &link : classStubs)
			LinkSuperclassStub(*link.first, *link.second, cm, peerStubs,
				TheRoot(), m_pRegistry);

		//0.9.10 D5 + 0.9.12 (v2.7): interfaces — statics, constants and
		//bodyless instance-method stubs.
		for (const auto &iface : cm.interfaces)
		{
			if (nameExistsInRoot(LeafNameOfKey(iface.name)))
				continue;  //R10-1
			SnInterfaceDecl *stub = CreateInterfaceStub(iface, cm, loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
		}

		//0.10.14: enums — value members + member-method stubs, bound
		//through the same FindModuleType channel as structs/classes.
		for (size_t ei = 0; ei < ownEnums; ++ei)
		{
			if (nameExistsInRoot(LeafNameOfKey(cm.enumKeys[ei])))
				continue;  //R10-1
			SnEnumDecl *stub = CreateEnumStub(cm.enumKeys[ei],
				cm.enumNames[ei], cm.enumMethodIndices[ei],
				cm.enumMethodAccess[ei], cm, loc);
			m_Tree.Root()->Members().push_back(stub);
			m_importedTypes.push_back(stub);
		}
	}

	//Stubs built for each CompiledFunction entry, in source-module order.
	//Caller (RegisterExternalStubs) tags each one with its module's
	//registry entry. Complete: it also holds the detached stubs (names
	//that already existed in root), which never became root members.
	const std::vector<SnFunction*>& ImportedFunctions() const
	{
		return m_importedFuncs;
	}

	//Take ownership of the detached stubs (names that already existed
	//in root — R10-1 collisions). They are registered in
	//ImportedFunctions() but are NOT root members; the caller keeps
	//them alive so the registry stub table stays valid until the
	//build ends.
	std::vector<std::unique_ptr<SnFunction>> TakeDetachedStubs()
	{
		return std::move(m_detachedStubs);
	}

	//Struct/class stubs minted into root, in source-module order.
	//Caller (RegisterExternalStubs) tags each with its module's
	//registry entry so FindModuleType can bind "<pkg>.<Type>" through
	//the owner-filtered root scan. Unlike ImportedFunctions there are
	//no detached type stubs: a collision means no stub was minted.
	const std::vector<SnField*>& ImportedTypes() const
	{
		return m_importedTypes;
	}

	const std::string& ModuleName() const
	{
		return m_moduleName;
	}

private:
	//Layer 6 policy: imported module must not define main().
	//Phase 5: the entry judgment uses the qualified key — a root main.n
	//in the imported module is keyed "<module>.main", so the bare "main"
	//comparison would miss it.
	void CheckNoMainFunction(const CompiledModule &cm)
	{
		const std::string entryKey = cm.name + ".main";
		for (const auto &cf : cm.functions)
		{
			if (cf.name == entryKey)
			{
				throw std::runtime_error(
					"imported module '" + cm.name +
					"' must not define main(); main() is reserved for the root module");
			}
		}
	}

	SyntaxTree &m_Tree;
	std::string m_moduleName;
	const ModuleRegistry *m_pRegistry;  //non-owning; enables the prelude superclass link
	std::vector<SnFunction *> m_importedFuncs;
	std::vector<SnField *> m_importedTypes;
	//Registered stubs that did NOT join the root (R10-1 collisions);
	//owned here until the caller takes them via TakeDetachedStubs().
	std::vector<std::unique_ptr<SnFunction>> m_detachedStubs;

	//Shortcut to the root namespace (avoids repeated m_Tree.Root() calls
	//in inner loops).
	SnNamespace &TheRoot() { return *m_Tree.Root(); }
};

} //namespace nlang
