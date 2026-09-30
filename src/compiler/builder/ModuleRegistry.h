/*---
ModuleRegistry.h - compile-time module registry of the NLang compiler.

Maps every translation unit to its dotted module path, tracks which
module owns each merged top-level symbol, and holds each unit's import
gate (module import visibility plan, spec §6).
---*/
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include "SyntaxNode.h"

namespace nlang
{

class SnField;
class SnFunction;
class TranslationUnit;
struct ImportSpec;

//Predicate deciding whether a single-segment name is a known library
//namespace (standard library OR a discovered third-party namespace).
//Injected by ModuleBuilder so the registry stays independent of the
//langservice symbol index (dependency inversion).
using LibraryNamespacePredicate =
	std::function<bool(const std::string&)>;

//Spec §7 module-not-found wording — single source shared by the gate
//resolver (BuildGate) and the .nmod loader (ModuleBuilder::LoadImports).
std::string ModuleNotFoundText(const std::string& moduleName);

//Library namespaces that are permanently taken by stdlib/*.n + nlang_<ns>.dll:
//a project directory or module with one of these names would shadow a
//standard library. The list lives here (the compiler's gate), not in the VM.
bool IsReservedLibraryName(const std::string& name);

/*
Compile-time module registry (spec §6): maps every translation unit
to its dotted module path (relative to BuildParams::m_sProjectDir),
tracks which module owns each merged top-level symbol (side table —
NF_ flag bits are fully allocated), and holds each unit's import gate
(exact paths + wildcard prefixes + same-directory
auto-inclusion). External .nmod modules join the registry with
isExternal=true (they own their stubs and never share a directory with
a TU). VM/bytecode are untouched: everything here is resolution-time
only.
*/
class ModuleRegistry
{
public:
	//Sentinel owner index: "no module owns this symbol / context".
	//Ownerless nodes are judged per side, so that policy stays outside
	//ShareBarePool: the duplicate check treats an ownerless candidate as
	//a shared-pool member — it shares every pool, so the legacy
	//name-equality conflict stays (DuplicateFieldChecker::SameBarePool)
	//— while IsBareVisible treats an ownerless context as defensively
	//invisible (it never passes the bare filter).
	static constexpr uint32_t NO_OWNER = 0xFFFFFFFFu;

	//--- per-TU import gates ---
	struct ImportGate
	{
		//Exact module paths (project TUs + external .nmod names).
		std::vector<std::string> exact;
		//Wildcard prefixes ("utils." — recursive, D5).
		std::vector<std::string> wildcards;
	};

	//Register a TU; moduleIndex == position in registration order.
	//isLibrary marks an inline library TU (from the search path): it is
	//compiled in but never a project module (no same-directory visibility).
	//Returns false after filling outErrors on a reserved path segment
	//(io/math/fs) — the caller logs and aborts the build.
	bool RegisterUnit(uint32_t moduleIndex, const TranslationUnit& tu,
		const std::string& projectDir,
		std::vector<std::string>& outErrors, bool isLibrary = false);

	//Register an external .nmod by name; returns its module index
	//(stubs get TagOwner'd with it by the caller). External
	//directories never equal TU directories (see DirectoryOf).
	uint32_t AddExternalModule(const std::string& name);

	//Resolve one TU's ImportSpec list into its gate. Priority per
	//spec §5.1: builtin → project module → external single-segment.
	//Same-directory project modules auto-added (D7). Wildcard only
	//matches project paths (external names are single-segment, §3.3).
	//Unresolved names land in outErrors (spec §7 wording); external
	//names to load are appended (deduped) to externalOut.
	bool BuildGate(uint32_t moduleIndex,
		const std::vector<ImportSpec>& specs,
		std::vector<std::string>& externalOut,
		std::vector<std::string>& outErrors,
		const LibraryNamespacePredicate& isLibraryNamespace);

	//"utils.helper" / "main"; empty before RegisterUnit.
	const std::string& ModulePathOf(uint32_t moduleIndex) const;
	//Drop every entry and owner tag. A second Build() on the same
	//builder must not append to the previous run's entries or keep
	//owner keys pointing into its destroyed AST.
	void Reset();
	//Directory part in dotted form: "" / "utils" / "utils.sub".
	//External modules: unique sentinel (never equals a TU dir).
	//Footgun: an out-of-range or NO_OWNER index also returns "" —
	//callers must rule those out before comparing directories.
	std::string DirectoryOf(uint32_t moduleIndex) const;
	bool IsExternal(uint32_t moduleIndex) const;
	//True for an inline library TU module (search-path <name>.n).
	bool IsLibraryModule(uint32_t moduleIndex) const;

	//NO_OWNER context (no tagged ancestor) never passes a gate: both
	//return false for moduleIndex == NO_OWNER — the gate queries are
	//NO_OWNER-safe (no m_modules[NO_OWNER] indexing).
	bool IsModuleImported(uint32_t moduleIndex,
		const std::string& dottedPath) const;
	//Module import visibility (D1/D7): shared "same bare pool" core —
	//bare-visible entities live in one pool per directory, so same-
	//directory modules always collide and cross-directory ones never do.
	//Callers must rule NO_OWNER out first (DirectoryOf contract); the
	//NO_OWNER policy differs per caller and stays outside (see the
	//NO_OWNER note above).
	bool ShareBarePool(uint32_t ownerA, uint32_t ownerB) const
	{
		if (ownerA == ownerB)
			return true;
		if (IsExternal(ownerA) || IsExternal(ownerB))
			return false;
		return DirectoryOf(ownerA) == DirectoryOf(ownerB);
	}
	//True when name is a TU compiled into this build (project module or an
	//inline library TU) — a known non-external module. The unified
	//"compiled in" surface after libraries are inlined.
	bool IsCompiledInModule(const std::string& dottedPath) const;
	//Project TU paths + external .nmod names (union).
	bool IsKnownModule(const std::string& dottedPath) const;
	//True when some known module path equals dottedPrefix or starts with
	//"dottedPrefix." — segment-aligned, so "utils" matches "utils.helper"
	//but not "utils2.x". Fires the class/module conflict hint (spec §6.2).
	bool HasKnownModuleStartingWith(const std::string& dottedPrefix) const;
	//Stub table of an external module (filled right after its .nmod
	//loads; consumed by ModuleFunctions).
	void SetExternalStubs(uint32_t moduleIndex,
		std::vector<SnFunction*> stubs);
	//Functions of a module matching calleeName: project module → root
	//top-level free functions owned by it; inline library TU → functions
	//inside namespace <path> owned by it; external module → same-name
	//subset of its stub table.
	std::vector<SnFunction*> ModuleFunctions(
		const std::string& path,
		const std::string& calleeName) const;

	//Find a type declaration (class/struct/enum/interface) named
	//typeName inside the compiled-in unit path. Returns null for an
	//external .nmod or when the unit/type is not present.
	SnField* FindModuleType(const std::string& path,
		const std::string& typeName) const;

	//Owner side table: tag a merged top-level member (also used for
	//namespace members one+ levels deep — namespaces can span TUs).
	void TagOwner(SnField& member, uint32_t moduleIndex);
	//Forget a member's owner tag. The table is pointer-keyed, so an
	//entry whose node dies (a merged-away namespace shell) must be
	//dropped: a later allocation reusing the address would silently
	//inherit the dead node's owner. Guarded by MergeTransUnits' sweep;
	//no direct unit test — standalone SnField nodes have no supported
	//lifetime outside the AST (hand new/delete of one crashes even with
	//no registry involved), so only the sweep's observable effects are
	//covered (namespaceCrossTUTagsEachSide).
	void EraseOwner(SnField& member);
	//NO_OWNER when the member carries no tag.
	uint32_t OwnerOf(const SnField& member) const;

	//Package name of a member: the OWNING unit's dotted path ("" when
	//unowned). Deliberately NOT derived from the AST namespace chain —
	//the container exists only where a library file literally writes
	//`namespace <path>`, so the chain cannot express a path-derived
	//package for project units. Call contract: valid only for top-level
	//members merged into root and for library members; class methods
	//keep their bare-name rule on the caller's side.
	std::string PackageOf(const SnField& member) const;
	//PackageOf, dotted with the member's name; the bare name when the
	//member has no package.
	std::string QualifiedName(const SnField& member) const;

	//Nearest ancestor (Parent() chain) carrying an owner tag — the
	//"current TU" for a resolver context (spec F18); NO_OWNER at root.
	uint32_t OwnerOfContext(const SyntaxNode& context) const;

	uint32_t ModuleCount() const
	{
		return static_cast<uint32_t>(m_modules.size());
	}
private:
	//A registered TU module path ("utils.helper") — externals are
	//matched by their single-segment .nmod name only.
	bool IsProjectModule(const std::string& dottedPath) const;

	//BuildGate's per-spec arm: resolves one ImportSpec into gate
	//entries (builtin / project / wildcard union / external candidate)
	//or an outErrors entry. See BuildGate for the priority contract.
	void ApplyImportSpec(const ImportSpec& spec, ImportGate& gate,
		std::vector<std::string>& externalOut,
		std::vector<std::string>& outErrors,
		const LibraryNamespacePredicate& isLibraryNamespace) const;
	//Non-wildcard arm of ApplyImportSpec: compiled-in module, signature-
	//only library fallback, external .nmod candidate, or an unresolvable
	//dotted path.
	void ApplyNonWildcardImport(const std::string& name, ImportGate& gate,
		std::vector<std::string>& externalOut,
		std::vector<std::string>& outErrors,
		const LibraryNamespacePredicate& isLibraryNamespace) const;

	//Same-name functions of a compiled-in module owned by moduleIndex. An
	//inline library TU keeps them in `namespace <path>`; a project module
	//uses root-level free functions.
	std::vector<SnFunction*> CompiledInFunctions(uint32_t moduleIndex,
		const std::string& path,
		const std::string& calleeName) const;

	//Wildcard arm of ApplyImportSpec (D11 union semantics): the exact
	//module "X" plus every "X."-prefixed project module. An empty union
	//is reported as an error (D10) instead of a silent no-op.
	void ApplyWildcardImport(const std::string& name, ImportGate& gate,
		std::vector<std::string>& outErrors) const;

	struct ModuleEntry
	{
		std::string path;      //"utils.helper" / "lib"
		bool isExternal = false;
		bool isLibrary = false; //inline library TU (search path), not a project module
		ImportGate gate;       //TU entries only (BuildGate)
	};
	std::vector<ModuleEntry> m_modules;
	std::unordered_map<const SnField*, uint32_t> m_ownerOf;
	//External module index → its function stubs (import visibility
	//plan: qualified calls resolve through the stub table, never
	//through the shared root).
	std::unordered_map<uint32_t, std::vector<SnFunction*>> m_externalStubs;
};

} //namespace nlang
