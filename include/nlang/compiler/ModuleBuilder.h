/*-----------------------------------------------------------------------------
	ncomp/intf/ModuleBuilder.h
	This file define the interface of the module builder of nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "BuildEnvironment.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nlang
{

//Internal builder type (src/compiler/builder/ModuleRegistry.h); opaque
//in this public header.
class ModuleRegistry;
//Internal stub-minting type (src/compiler/builder/
// CompiledModuleNodeBuilder.hpp); reference use below only.
class CompiledModuleNodeBuilder;

//A tool for compiling some nlang source files to a binary module.
class NLANG_COMPILER_API ModuleBuilder
{
public:
	ModuleBuilder(const BuildParams&, CompileLogger&);

	~ModuleBuilder();

	//Compile source files to a module file.
	bool Build();

	//Phase 6 linked build (the production path): per-unit images
	//(BuildUnitImages) closed under nlink into ONE runnable module,
	//written as the single .ncu artifact. The linked artifact is a
	//plain self-contained module — placeholders are resolved and the
	//import tables empty — so today's loader/executor run it unchanged.
	//Project-mode builds (BuildParams.m_bProjectMode, ncc -p)
	//additionally write the .npkg distribution archive beside the
	//.ncu (member and entry record carry the artifact's own identity);
	//single-file builds stop at the .ncu. Link diagnostics (unresolved
	//symbols) are logged like compile errors. Returns false on any
	//front-end, codegen, link, or artifact-write error.
	bool BuildLinked();

	//Phase 6 per-unit build result: one CompiledModule image per
	//translation unit (entry-owning unit first, then TU order), the
	//loaded external .ncu imports (NOT merged in — they ride along for
	//the load-time linker), and the program entry's qualified key
	//("" = none or ambiguous, already logged).
	struct UnitImages
	{
		std::vector<CompiledModule> units;
		std::vector<CompiledModule> external;
		std::string entryKey;
	};

	//Phase 6: run the front end as Build() does, then generate code PER
	//TRANSLATION UNIT (VmBackend::GenerateUnits) instead of one merged
	//module. Every cross-unit reference is a placeholder slot + import
	//record the caller's load-time linker resolves (NcuLinker::Link over
	//units + external). Empty units = build errors (already logged).
	UnitImages BuildUnitImages();

	//Compile-time module registry; populated with one entry per
	//translation unit (and per imported .ncu) during Build().
	const ModuleRegistry& Registry() const;

	//Read-only view of the merged syntax-tree root (tests and tooling;
	//internal passes use TreeRoot()).
	const SnNamespace& TreeRootView() const
	{
		assert(TheAST().Root());
		return *TheAST().Root();
	}
private:
	//Get the root of the syntax tree.
	SnNamespace &TreeRoot()
	{
		assert(TheAST().Root());
		return *TheAST().Root();
	}

	//Create a empty module as the current module to be built.
	bool CreateModule();
	//Reset the shared syntax tree and install the built-in type
	//singletons. Must run before parsing (the parser resolves built-in
	//type names against them).
	void InitSyntaxTree();
	//Front-end orchestration: create, init, parse, discover library
	//sources, register, expand aliases, then load imports.
	bool PrepareUnits();
	//Merge TU roots and run every resolution pass; false after errors.
	bool ResolveAll();
	//Register every TU's module path in the registry. False after
	//logging the registration errors.
	bool RegisterUnits();
	//Load imported symbols to a rebuilt AST.
	bool LoadImports();
	//Discover every import-reachable library <pkg>.n source (imports
	//iterated to a fixed point so a library can depend on a library):
	//index its signatures and parse it fully as an inline library
	//translation unit. Dotted names map to <root>/<a/b/c.n>; idempotent
	//per file.
	void DiscoverLibraryUnits();
	//Locate the library source for a dotted package name:
	//`a.b.c` is <root>/a/b/c.n on the first matching search dir. Returns
	//(file, matched root); the file is empty when no root has it.
	std::pair<std::string, std::string> FindLibrarySourceFile(
		const std::string& dottedName) const;
	//Every (file, root) match across the effective library dirs — the
	//duplicate-package diagnosis scans the full set instead of stopping
	//at the first hit.
	std::vector<std::pair<std::string, std::string>>
	FindLibrarySourceMatches(const std::string& dottedName) const;
	//One import's library-source handling (DiscoverLibraryUnits' inner
	//loop body): the multi-root duplicate diagnosis, otherwise inline the
	//single match under its root. True when a TU was added.
	bool HandleLibraryImport(const std::string &dottedName);
	//Index and fully parse one library source file as a library TU,
	//recording the root it was found under (the package derives from
	//that root, not from the project dir). True when newly inlined.
	bool ParseLibraryUnit(const std::string& path,
		const std::string& packageRoot);
	//Build the per-TU import gates (D1: imports are file-scoped) and
	//collect the single-segment external .ncu candidates into
	//rExternalNames. False after logging the gate errors.
	bool BuildImportGates(std::vector<std::string> &rExternalNames);
	//Load one external .ncu candidate: parse it, mint the stub nodes,
	//register their owners and keep the detached stubs alive. False
	//after logging the failure.
	bool LoadExternalModule(const std::string &name);
	//Register a loaded module's stub functions in the VmBackend
	//side-table and the registry's external entry; detached stubs
	//(name clashes) stay owned by the builder.
	void RegisterExternalStubs(CompiledModuleNodeBuilder &builder,
		uint32_t srcModIdx, const std::string &name);
	//Find .ncu file for a module name in m_ImportDirs. Returns empty if not found.
	std::string FindModuleFile(const std::string& name) const;
	//Generated executable codes.
	bool GenerateCodes();
	//Save the current module to a file.
	bool SaveModule();
	//Pack the linked module as the .npkg distribution archive beside
	//the .ncu (project-mode builds; BuildLinked).
	bool WritePackageArtifact(const CompiledModule& linked,
		const std::string& entryKey);
	//Roughly parse the source files as translation units.
	void ParseTransUnits();
	//Phase 13: register `using N = T;` aliases and expand them at use
	//sites. Runs per translation unit after parsing and before merging —
	//alias scope is the translation unit, and the unit boundaries (and
	//roots) are gone after the merge.
	void ExpandTypeAliases();
	//Merge the root fields in different translation units into the AST.
	void MergeTransUnits();
	//Tag a (unit-root or nested) namespace's direct members with the
	//owning module index; nested namespaces recurse (they can span TUs).
	void TagUnitMembers(SnNamespace &ns, uint32_t moduleIndex);
	//Erase the owner tags of the nodes that did not survive the merge
	//and die with the unit root (merged-away namespace shells) — the
	//owner table must not keep entries for dying nodes.
	void EraseUnitOwners(SnNamespace &ns);

	void ResolveUsingLists();
	//Resolve type names of data fields in the syntax tree, but skip the 
	//function statements.
	void ResolveDataTypes();

	void CheckDuplicateFields();

	void ResolveDataValues();

	void ResolveStatements();

	//Phase 13: report every function reference still pending after all
	//consumers ran (never met an expected Func type).
	void SweepPendingFuncRefs();

	//Check for circular struct references.
	void CheckStructCircularRefs();

		//Check for circular class inheritance.
		void CheckClassCircularInheritance();

	//Verify each class declaring "implements IFoo" provides all methods
	//declared in IFoo (matching name, params, return type). Reports errors
	//for missing methods.
	void CheckInterfaceImplementation();

	//Build LLVM data types.
	void GenerateTypeFields();

	void GenerateStatements();

	bool ResolveNameExpr(SnNameExpr &, SnField *pContext, 
		const SnField &accessor);

	bool ResolveIdentifierExpr(SnIdentifierExpr &, SnField *pContext, 
		const SnField &accessor);

	bool ResolveMemberExpr(SnMemberExpr &, SnField *pContext, 
		const SnField &accessor);

	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor);
	void GenerateDataFields();

	std::unique_ptr<BuildEnvironment> m_upEnv;
	std::unique_ptr<PtrList<TranslationUnit>> m_upTransUnits;
	//Cross-module import infrastructure (Phase 9c follow-up): compiled
	//modules loaded from .ncu files during LoadImports. Ownership is
	//transferred to VmBackend at the start of GenerateCodes via
	//SetImportedModules(); the backend then merges them into the user
	//module during GenerateStatements.
	//Absolute paths of library .n files already parsed inline this build,
	//so each is fully parsed at most once (independent of the signature
	//index, which loads the standard library at construction).
	std::unordered_set<std::string> m_inlinedLibraryFiles;
	//Library source abs path -> the search root it was found under
	//(the matched root is the package-name base for that TU).
	std::unordered_map<std::string, std::string> m_librarySourceRoots;
	//The project source files, normalized: discovery must not re-inline
	//a <root>/<pkg>.n candidate that is already a project TU (it would
	//register the same package twice).
	std::unordered_set<std::string> m_projectSourceFiles;
	std::vector<CompiledModule> m_loadedImports;
	//External function stubs whose name already existed in the root
	//(e.g. two .ncu modules exporting the same function). They are
	//registered in the VmBackend side-table and their module's registry
	//stub table, but are NOT root members — the builder owns them so
	//both tables stay valid until the build ends.
	std::vector<std::unique_ptr<SnFunction>> m_upDetachedImportStubs;
};

} //namespace nlang
