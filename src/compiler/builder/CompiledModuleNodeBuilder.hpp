#pragma once
#include "CompiledModuleCtorStub.hpp"
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
	//Construct a builder for one source module.
	CompiledModuleNodeBuilder(SyntaxTree &tree,
		const std::string &moduleName) :
		m_Tree(tree), m_moduleName(moduleName)
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

		//Functions first so that subsequent class/struct stub members can
		//reference them by name if needed (resolver does name-based lookup
		//later, so physical order among siblings doesn't matter — but having
		//functions first matches the typical parser emission order).
		for (uint32_t i = 0; i < ownFuncs; ++i)
		{
			if (methodOrCtorIndices.count(i))
				continue;  //class method/ctor — owned by its class, not free
			SnFunction *stub = CreateFunctionStub(cm.functions[i], cm, loc);
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

		//Classes. 0.9.10 D5: each stub rebuilds its exported public
		//statics (CreateClassStub); the super links run after the loop so
		//a subclass stub can point at its peer stub regardless of
		//declaration order.
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
			LinkSuperclassStub(*link.first, *link.second, cm, peerStubs);

		//0.9.10 D5: interfaces — static-member stubs only.
		for (const auto &iface : cm.interfaces)
		{
			if (nameExistsInRoot(LeafNameOfKey(iface.name)))
				continue;  //R10-1
			SnInterfaceDecl *stub = CreateInterfaceStub(iface, cm, loc);
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
	//Build a SnFunction stub for an imported CompiledFunction.
	//The stub has no Body() — VmBackend.RegisterFunctions and
	//GenerateAllBytecode already skip bodyless functions, and the
	//NF_Imported flag adds an explicit IsImported() gate for safety.
	SnFunction *CreateFunctionStub(const CompiledFunction &cf,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		//Return type. RTK_Void → no return type (SnFunction::HasReturn() == false).
		//v1.12: the serialized descriptor supplies the true type; the
		//NonSerialized sentinel (func/interface returns) degrades to the
		//int32 placeholder.
		SnFieldExpr *pRetType = nullptr;
		if (cf.returnTypeKind != RTK_Void)
			pRetType = SynthTypeExprFromDesc(cf.returnTypeDesc, cm, loc);

		//Synthesize paramCount SnFormalParam nodes. v1.12: each formal's true
		//type and out flag come from the serialized descriptors (free
		//functions carry exactly paramCount of them — methods never reach
		//stub construction, the methodOrCtorIndices filter owns those).
		//Option B: if cf.defaultValues[i] carries a constant-foldable default,
		//attach the reconstructed SnLiteralExpr to the stub formal so the
		//resolver can apply it at consumer call sites.
		//Use raw PtrList<SnFormalParam>* — SnFunction takes UniquePtrList by
		//value, whose inner_collection* constructor assumes ownership and
		//deletes the source list.
		auto *pParams = new PtrList<SnFormalParam>();
		//Formal names from the producer's frame-layout locals table
		//(ModuleSaver serializes every AllocLocal name with its isParam
		//flag; for free functions those entries are exactly the params,
		//in declaration order). Named-argument binding (BindNamedActual)
		//matches actuals to formals by name, so the former "p0"/"p1"
		//placeholders made every cross-module named call unbindable.
		//Missing/misaligned entries fall back to the placeholders —
		//stub construction never rejects a module over naming.
		std::vector<std::string> formalNames;
		for (const auto &ld : cf.locals)
			if (ld.isParam)
				formalNames.push_back(ld.name);
		//Effective per-formal names (locals entries with the placeholder
		//fallback), shared by the param nodes and the v2.3 ExprDesc
		//formal-reference rebuild — a rebuilt `b = a * 2` must name the
		//formals exactly as the stub's own params do.
		std::vector<std::string> stubFormalNames(cf.paramCount);
		for (uint16_t i = 0; i < cf.paramCount; ++i)
			stubFormalNames[i] =
				(i < formalNames.size() && !formalNames[i].empty())
					? formalNames[i]
					: "p" + std::to_string(i);
		for (uint16_t i = 0; i < cf.paramCount; ++i)
		{
			auto paramName = new std::string(stubFormalNames[i]);
			NodeBits paramFlags = NF_NONE;
			SnFieldExpr *pParamType;
			if (i < cf.paramTypeDescs.size())
			{
				const ParamTypeDesc &ptd = cf.paramTypeDescs[i];
				if (ptd.flags & PTDF_Out)
					paramFlags |= NF_Out;
				pParamType = SynthTypeExprFromDesc(ptd.type, cm, loc);
			}
			else
			{
				pParamType = new SnIdentifierExpr(NK_Int32, loc);
			}
			SnExpression *pDefault = nullptr;
			if (i < cf.defaultValues.size())
				pDefault = SynthDefaultExpr(cf.defaultValues[i], cm,
					cf.name, i, stubFormalNames, loc);
			auto *pParam = new SnFormalParam(paramFlags, pParamType, paramName,
				pDefault, loc);
			pParams->push_back(pParam);
		}

		//Mint the function name as a heap string (SnFunction takes ownership).
		//Leaf name: see LeafNameOfKey (phase 5 key vs. stub-name split).
		auto pName = new std::string(LeafNameOfKey(cf.name));

		auto *pFunc = new SnFunction(FA_Public, NF_Data, pRetType, pName,
			pParams, loc);
		//R6-2: explicitly add NF_Imported. SnFunction's constructor does
		//not auto-set this flag (unlike SnIdentifierExpr). Without it,
		//RegisterStructs/Classes/.../PopulateClassMethods' IsImported()
		//gates would not skip these stubs.
		pFunc->AddFlags(NF_Imported);
		//0.9.10 D5: the static flag rides the record — the resolver's
		//receiver-namespace split (MatchesReceiverForm) and the emitter's
		//slot arity (FunctionSlotFor: statics take no this slot) both key
		//on it.
		if (cf.isStatic)
			pFunc->AddFlags(NF_Static);

		return pFunc;
	}

	//Build a SnStructDecl stub for an imported CompiledStruct.
	//Fields stay wire-only — VmBackend.RegisterStructs skips imported
	//stubs entirely (R3 + IsImported gating) and external struct FIELD
	//access is out of scope. 0.9.11 v2.6: the CONSTRUCTORS are rebuilt
	//(from constructorIndices, declaration order) — a cross-module
	//`pkg.Vec(3,4)` needs member declarations to bind against; the call
	//itself emits an imported function slot, never local bytecode.
	SnStructDecl *CreateStructStub(const CompiledStruct &cs,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		//Field info stays in the loaded CompiledModule, which rides along
		//to the load-time linker as a peer image.
		//SnStructDecl takes UniquePtrList<SnField> by value; pass a raw
		//PtrList* whose ownership is transferred via the implicit
		//inner_collection* constructor.
		auto *pMembers = new PtrList<SnField>();
		for (auto idx : cs.constructorIndices)
		{
			//Out-of-range is a corrupt image — the loader validated the
			//counts, the bound check keeps stub minting defensive.
			if (idx < cm.functions.size())
				pMembers->push_back(
					CreateMethodStub(cm.functions[idx], cm, loc));
		}
		auto pName = new std::string(LeafNameOfKey(cs.name));
		auto *pDecl = new SnStructDecl(pName, pMembers, loc);
		pDecl->AddFlags(NF_Imported);   //R6-2
		return pDecl;
	}

	//Build a SnClassDecl stub for an imported CompiledClass.
	//Instance fields/methods stay wire-only — VmBackend skips imported
	//class registration and a stub cannot host a receiver anyway.
	//0.9.10 D5: the PUBLIC static members ARE rebuilt (from
	//publicStaticMethodIndices) — a static call needs no receiver, only a
	//declaration to bind against; private statics stay invisible.
	//0.9.11 v2.6: the CONSTRUCTORS are rebuilt too (from
	//constructorIndices, declaration order) — `new pkg.Calc(21)` /
	//`pkg.Calc(21)` bind against these member stubs; the call emits an
	//imported function slot. Access is not carried on the wire, so every
	//exported ctor stub is public at the consumer.
	SnClassDecl *CreateClassStub(const CompiledClass &cc,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		auto pName = new std::string(LeafNameOfKey(cc.name));
		auto *pMembers = new PtrList<SnField>();
		for (auto idx : cc.publicStaticMethodIndices)
		{
			//Out-of-range is a corrupt image — the loader validated the
			//counts, the bound check keeps stub minting defensive.
			if (idx < cm.functions.size())
				pMembers->push_back(
					CreateFunctionStub(cm.functions[idx], cm, loc));
		}
		for (auto idx : cc.constructorIndices)
		{
			if (idx < cm.functions.size())
				pMembers->push_back(
					CreateMethodStub(cm.functions[idx], cm, loc));
		}
		auto *pDecl = new SnClassDecl(pName, nullptr, pMembers, loc);
		pDecl->AddFlags(NF_Imported);   //R6-2
		return pDecl;
	}

	//0.9.10 D5: build a SnInterfaceDecl stub — the static members are
	//the only content (interface instance members are abstract; the
	//producer exports nothing else).
	SnInterfaceDecl *CreateInterfaceStub(const CompiledInterface &ci,
		const CompiledModule &cm, const ISourceLocation &loc)
	{
		auto pName = new std::string(LeafNameOfKey(ci.name));
		auto *pMembers = new PtrList<SnField>();
		for (auto idx : ci.staticMethodIndices)
		{
			if (idx < cm.functions.size())
				pMembers->push_back(
					CreateFunctionStub(cm.functions[idx], cm, loc));
		}
		//0.9.10 D3: exported constants rebuild as `public static const`
		//fields whose synthesized literal initializer folds again at the
		//consumer's use sites (the declaration contract already ran at
		//the producer; imported stubs are exempt from re-checking).
		for (const auto &constant : ci.constants)
		{
			SnExpression *pInit = SynthScalarDefault(constant.value, cm, loc);
			if (!pInit)
				throw std::runtime_error(
					"imported interface '" + ci.name
					+ "' has a malformed constant value record");
			auto *pField = new SnClassField(
				SynthTypeExprFromDesc(constant.type, cm, loc),
				new std::string(constant.name), FA_Public, loc, pInit);
			pField->AddFlags(NF_Static);
			pField->AddFlags(NF_Const);
			pMembers->push_back(pField);
		}
		auto *pDecl = new SnInterfaceDecl(pName, pMembers, loc);
		pDecl->AddFlags(NF_Imported);   //R6-2
		return pDecl;
	}

	//0.9.10 D5: point a class stub's SuperClass at its peer stub from the
	//same module so inherited statics resolve through the normal chain
	//walk. A super from another imported module or a builtin (Exception
	//family) falls back to the consumer root's imported/builtin entry; a
	//consumer-owned class of the same name is NOT hijacked.
	void LinkSuperclassStub(SnClassDecl &stub, const CompiledClass &cc,
		const CompiledModule &cm,
		const std::unordered_map<std::string, SnClassDecl*> &peerStubs)
	{
		if (cc.superClassIdx < 0
			|| cc.superClassIdx >= static_cast<int16_t>(cm.classes.size()))
			return;
		const std::string superLeaf =
			LeafNameOfKey(cm.classes[cc.superClassIdx].name);
		auto it = peerStubs.find(superLeaf);
		if (it != peerStubs.end())
		{
			stub.SuperClass(it->second);
			return;
		}
		SnField *pRoot = TheRoot().FindField(superLeaf);
		if (pRoot && pRoot->Kind() == NK_ClassDecl)
		{
			auto *pRootClass = static_cast<SnClassDecl*>(pRoot);
			if (pRootClass->ContainFlags(NF_Imported)
				|| pRootClass->IsBuiltinClass())
				stub.SuperClass(pRootClass);
		}
	}

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
