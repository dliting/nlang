/*-----------------------------------------------------------------------------
	ncomp/intf/ModuleBuilder.h
	This file define the interface of the module builder of nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "BuildEnvironment.h"
#include "SyntaxTree.h"
#include "nlang/vm/CompiledModule.h"
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Internal builder type (src/compiler/builder/ModuleRegistry.h); opaque
//in this public header.
class ModuleRegistry;
//AST node types (SnData.h / SnStatements.h); pointer use below only.
class SnFunction;
class SnParagraph;
//Internal stub-creation type (src/compiler/builder/
// CompiledModuleNodeBuilder.hpp); reference use below only.
class CompiledModuleNodeBuilder;
//Reserved prelude package record (src/compiler/BuiltinPrelude.h);
//reference use below only.
struct PreludePackage;

//A tool for compiling some nlang source files to a binary module.
class NLANG_COMPILER_API ModuleBuilder
{
public:
	ModuleBuilder(const BuildParams&, CompileLogger&);

	~ModuleBuilder();

	//Phase 6 artifact build (the production path): write the per-unit
	//images AS the artifacts — linking moved to load time (nloader +
	//nlink; design section 3). Single-file mode writes the entry unit
	//image as the .ncu (its import slots ride along; library code joins
	//the closure at run time); project mode (BuildParams.
	//m_bProjectMode, ncc -p) packs every unit image as a member of the
	//.npkg distribution archive and writes no .ncu. Loaded external
	//imports are not written — the consumer's import slots name them
	//and the run-time search path locates them. Returns false on any
	//front-end, codegen, or artifact-write error.
	bool BuildArtifacts();

	//Phase 6 per-unit build result: one CompiledModule image per
	//NON-LIBRARY translation unit (library TUs are signature-only and
	//ship in their own packages; entry-owning unit first, then TU
	//order), the loaded external .ncu/.npkg imports (NOT merged in —
	//they ride along for the load-time linker), and the program entry's
	//qualified key ("" = none or ambiguous, already logged).
	struct UnitImages
	{
		std::vector<CompiledModule> units;
		std::vector<CompiledModule> external;
		std::string entryKey;
	};

	//Phase 6: run the front end, then generate code PER TRANSLATION UNIT
	//(VmBackend::GenerateUnits). Every cross-unit reference is a
	//placeholder slot + import record the caller's load-time linker
	//resolves (NcuLinker::Link over units + external). Empty units =
	//build errors (already logged).
	UnitImages BuildUnitImages();

	//Compile-time module registry; populated with one entry per
	//translation unit (and per imported .ncu) during the build.
	const ModuleRegistry& Registry() const;

	//Read-only view of the merged syntax-tree root (tests and tooling;
	//internal passes use TreeRoot()).
	const SnNamespace& TreeRootView() const
	{
		NLANG_ASSERT(TheAST().Root());
		return *TheAST().Root();
	}
private:
	//Get the root of the syntax tree.
	SnNamespace &TreeRoot()
	{
		NLANG_ASSERT(TheAST().Root());
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
	//Register the artifact units found at discovery, build the per-TU
	//import gates against the complete module set, then give the
	//artifact units their stubs.
	bool LoadImports();
	//Discover every import-reachable library unit (imports iterated to
	//a fixed point so a library can depend on a library). A name with a
	//library source is indexed and parsed as an inline library
	//translation unit (dotted names map to <root>/<a/b/c.n>; idempotent
	//per file); a name no source answers for is looked up as an artifact
	//(DiscoverArtifactUnit) and queued for the load phase. The fixed
	//point also reads each queued image's import records (a mirror's
	//dependency chain joins the build without any TU naming it), and
	//names whose records nothing answers for are reported here.
	void DiscoverLibraryUnits();
	//DiscoverLibraryUnits' iteration: scan each TU's import specs, then
	//each artifact image queued since the last pass (its import-record
	//names resolve through ResolveMirrorImportName), until a pass adds
	//no TU and queues no artifact. specNames collects every non-wildcard
	//spec name (the import gate owns their not-found reports);
	//homelessNames collects record names nothing answered for, in
	//first-seen order.
	void RunDiscoveryFixedPoint(std::set<std::string> &specNames,
		std::vector<std::string> &homelessNames);
	//One fixed-point pass's mirror half: the artifact images queued
	//since scannedArtifacts was set have their import-record names
	//resolved, each name once. True when the pass grew the TU set or
	//the pending set (the fixed point iterates on growth).
	bool ScanMirrorImportRecords(size_t &scannedArtifacts,
		std::set<std::string> &mirrorNames,
		std::vector<std::string> &homelessNames);
	//The fixed point's closing report: every homeless record name gets
	//the module-not-found diagnostic (no gate sees it — no TU spec
	//names it), except names a TU spec carries, whose report the gate
	//already owns.
	void ReportHomelessMirrorNames(const std::set<std::string> &specNames,
		const std::vector<std::string> &homelessNames) const;
	//Locate the library source for a dotted package name:
	//`a.b.c` is <root>/a/b/c.n on the first matching search dir. Returns
	//(file, matched root); the file is empty when no root has it.
	std::pair<std::string, std::string> FindLibrarySourceFile(
		const std::string& dottedName) const;
	//One import's library-source handling (DiscoverLibraryUnits' inner
	//loop body): the multi-root duplicate diagnosis, otherwise inline the
	//single match under its root. A name no source answers for goes to
	//the artifact arm, unless a project source derives to it (that unit
	//registers under the name; no artifact may answer for it). True
	//when a TU was added. discoverArtifact=false runs the source arm
	//alone: the mirror-record resolver (ResolveMirrorImportName) calls
	//the artifact arm itself so it can read the outcome.
	bool HandleLibraryImport(const std::string &dottedName,
		bool discoverArtifact = true);
	//Resolve one import-record name read from a loaded mirror image,
	//in the import order (D2): prelude and project units own their
	//names outright; otherwise the library-source arm (a TU was added,
	//or a source answering for the name is already inlined), then the
	//artifact arm's outcome. True when the name found a home; false
	//means nothing answers for it (the caller reports it).
	bool ResolveMirrorImportName(const std::string &name);
	//True when a project source of this build derives to the module
	//path (the registry is not built yet at discovery).
	bool IsProjectModuleName(const std::string& moduleName) const;
	//Outcome of one name's artifact discovery (DiscoverArtifactUnit):
	//Queued — the unit was queued now or earlier; Refused — an artifact
	//answered but was rejected (the diagnosis is already logged);
	//Absent — nothing answers for the name.
	enum class ArtifactOutcome { Queued, Refused, Absent };
	//The artifact arm of one import name: the unit file
	//<dir>/<name>.ncu, else the member tables of the dir's packages,
	//across the effective library dirs in order. A found unit is queued
	//once however many units import the name; a refused one (unreadable
	//bytes, a header naming another module) is reported here.
	ArtifactOutcome DiscoverArtifactUnit(const std::string& name);
	//True when an artifact unit of that module path is already queued.
	bool HasPendingArtifact(const std::string& name) const;
	//Register the registry entry of every queued artifact unit. Runs
	//before the import gates are built, so a gate finds an artifact unit
	//as a registered name.
	void MaterializeArtifacts();
	//Discovery of one wildcard import's library union (D-29 rule 2): the
	//package's own <root>/a/b.n plus every unit under <root>/a/b/ in each
	//effective library dir, each through HandleLibraryImport. True when a TU
	//was added.
	bool DiscoverLibrarySubtree(const std::string &dottedName);
	//The same union's artifact arm (ModuleBuilderExternalImports.cpp):
	//every package member whose path is the name itself or lies under it
	//joins the build through HandleLibraryImport, which decides what a
	//member answers as (a source wins, a project name is skipped).
	bool DiscoverMemberTableSubtree(const std::string &dottedName);
	//HandleLibraryImport's match tail, shared with the prelude
	//resolution: a match that IS a project source is already compiled in;
	//anything else inlines under its root. True when a TU was added.
	bool ImportLibraryMatch(
		const std::pair<std::string, std::string>& match);
	//One reserved prelude package's implicit-import resolution: first
	//disk match wins (dev-iteration edits of the stdlib copy), the
	//embedded copy parses as a pseudo-path unit when no search root
	//provides it, and two roots offering the package is the
	//duplicate-package diagnosis. Shared by the implicit loop below and
	//an explicit import of a reserved name.
	bool EnsurePreludePackageImported(const PreludePackage& pkg);
	//The implicit prelude import (JLS §7.5.3 analogue): every reserved
	//prelude package (kPreludePackages), table order. Runs after
	//ParseTransUnits, before DiscoverLibraryUnits.
	bool EnsurePreludeImported();
	//Index and fully parse one library source file as a library TU,
	//recording the root it was found under (the package derives from
	//that root, not from the project dir). pSourceText parses in-memory
	//text instead of opening the file (the embedded prelude copy; no
	//signature index — the package classifies through its registry
	//entry). True when newly inlined.
	bool ParseLibraryUnit(const std::string& path,
		const std::string& packageRoot,
		const char* pSourceText = nullptr);
	//Build the per-TU import gates (D1: imports are file-scoped). False
	//after logging the gate errors.
	bool BuildImportGates();
	//v0.12.6 Task 8 (D13) workers (ModuleBuilderImportCycles.cpp):
	//CollectImportGraphEdges builds the dependency edge table (compiled
	//units with wildcard expansion + loaded images' import records);
	//RejectImportCycles walks each root through ImportCycleCheck and
	//logs the first cycle found. False after logging it.
	std::map<std::string, std::vector<std::string>>
		CollectImportGraphEdges();
	bool RejectImportCycles();
	//Load phase of the queued artifact units: build the stub nodes from
	//the images read at discovery and fill their registry entries. False
	//after logging the failure.
	bool LoadExternalModules();
	//Register a loaded unit's stub functions in its registry entry;
	//detached stubs (name clashes) stay owned by the builder.
	void RegisterExternalStubs(CompiledModuleNodeBuilder &builder,
		uint32_t extIdx);
	//Pack every unit image as a member of the .npkg distribution
	//archive (project-mode builds; BuildArtifacts). Member names are
	//the units' own module paths; the entry record names the
	//entry-owning unit and function.
	bool WritePackageArtifact(const std::vector<CompiledModule>& units,
		const std::string& entryKey) const;
	//Roughly parse the source files as translation units.
	void ParseTransUnits();
	//v0.9.1 script mode: wrap each non-library unit's top-level statement
	//paragraph in a synthesized int main(). Runs after RegisterUnits
	//(library identity known) and before ExpandTypeAliases (alias
	//expansion must reach the wrapped body).
	void SynthesizeScriptEntries();
	//Build the synthesized entry function around one unit's paragraph
	//(takes ownership of pScript as the function body). Mirrors the
	//handwritten bare "int main()" form exactly.
	SnFunction* BuildScriptEntry(SnParagraph* pScript);
	//Phase 13: register `using N = T;` aliases and expand them at use
	//sites. Runs per translation unit after parsing and before merging —
	//alias scope is the translation unit, and the unit boundaries (and
	//roots) are gone after the merge.
	void ExpandTypeAliases();
	//Merge the root fields in different translation units into the AST.
	//Runs in PrepareUnits BEFORE LoadImports: the import-stub collision
	//checks (nameExistsInRoot) must see the consumer's own declarations,
	//or a peer image's prelude records (own in every image by design)
	//would be created into the root beside them. TU order (user, prelude,
	//libraries) is the root member order — the shadowing contract (user
	//declarations ahead of the prelude) rides it.
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

	//Run the duplicate-field pass; true when it reported a synthesized
	//script entry (NF_ScriptEntry) clashing with a user-written top-level
	//main() in the same unit (D8), so ResolveAll can abort before statement
	//resolution cascades further errors.
	bool CheckDuplicateFields();

	void ResolveDataValues();

	void ResolveStatements();

	//Phase 13: report every function reference still pending after all
	//consumers ran (never met an expected func type).
	void SweepPendingFuncRefs();

	//Capture promotion (0.13.10): discovery walk + display-class
	//synthesis over the merged tree, before any codegen. Most failures
	//are invariant breaks, but two shapes are reachable user code
	//(expression-body nesting, receiver capture in an unsupported
	//context) — they report through the regular error channel. False
	//(with errors logged) aborts the unit build.
	bool RunCapturePromotion();

	//Check for circular struct references.
	void CheckStructCircularRefs();

		//Check for circular class inheritance.
		void CheckClassCircularInheritance();

	//Method-contract checking: reject any same-name override whose
	//signature differs from the ancestor virtual / interface method it
	//claims (name-based dispatch would read arguments bit-wrong), and
	//verify each class declaring "implements IFoo" provides all methods
	//declared in IFoo with fully identical signatures.
	void CheckMethodContracts();

	bool ResolveNameExpr(SnNameExpr &, SnField *pContext,
		const SnField &accessor);

	bool ResolveIdentifierExpr(SnIdentifierExpr &, SnField *pContext, 
		const SnField &accessor);

	bool ResolveMemberExpr(SnMemberExpr &, SnField *pContext, 
		const SnField &accessor);

	SnField *FindFieldInAncestor(const std::string &sName, SyntaxNode &parent,
		const SnField &accessor);

	std::unique_ptr<BuildEnvironment> m_upEnv;
	std::unique_ptr<PtrList<TranslationUnit>> m_upTransUnits;
	//Cross-module import infrastructure (Phase 9c follow-up): external
	//units read from unit files and packages during the import pipeline.
	//Kept whole — never merged into the build's units; BuildUnitImages
	//moves them into UnitImages.external so the load-time linker
	//resolves them as peer images.
	//Absolute paths of library .n files already parsed inline this build,
	//so each is fully parsed at most once (independent of the signature
	//index, which loads the standard library at construction).
	std::unordered_set<std::string> m_inlinedLibraryFiles;
	//Library source abs path -> the search root it was found under
	//(the matched root is the package-name base for that TU).
	std::unordered_map<std::string, std::string> m_librarySourceRoots;
	//The project source files, normalized: discovery must not re-inline
	//a <root>/<pkg>.n candidate that is already a project TU (it would
	//register the same package twice), and no artifact may answer for a
	//name one of them derives to.
	std::unordered_set<std::string> m_projectSourceFiles;
	//An artifact unit (a unit file or a package member) found at
	//discovery: its image is read, its registry entry is not built yet.
	//name is the unit's module path.
	struct PendingArtifactLoad
	{
		std::string name;
		CompiledModule image;
		//The unit file, or the package carrying the member.
		std::string artifactPath;
		//The unit's registry entry; MaterializeArtifacts sets it.
		uint32_t extIdx = 0;
	};
	std::vector<PendingArtifactLoad> m_pendingArtifacts;
	//The artifact units after their load phase — never merged into the
	//build's units; BuildUnitImages moves them into UnitImages.external
	//so the load-time linker resolves them as peer images.
	std::vector<CompiledModule> m_loadedImports;
	//External function stubs whose name already existed in the root
	//(e.g. two .ncu modules exporting the same function). They are
	//registered in their module's registry stub table, but are NOT root
	//members — the builder owns them so the table stays valid until the
	//build ends.
	std::vector<std::unique_ptr<SnFunction>> m_upDetachedImportStubs;
};

} //namespace nlang
