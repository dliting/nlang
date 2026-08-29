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
#include <cstdint>
#include "SyntaxNode.h"

namespace nlang
{

class SnField;
class SnFunction;
class TranslationUnit;
struct ImportSpec;

//Spec §7 module-not-found wording — single source shared by the gate
//resolver (BuildGate) and the .nmod loader (ModuleBuilder::LoadImports).
std::string ModuleNotFoundText(const std::string& moduleName);

/*
Compile-time module registry (spec §6): maps every translation unit
to its dotted module path (relative to BuildParams::m_sProjectDir),
tracks which module owns each merged top-level symbol (side table —
NF_ flag bits are fully allocated), and holds each unit's import gate
(exact paths + wildcard prefixes + builtin namespaces + same-directory
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
	//a shared-pool member — it always coexists (DuplicateFieldChecker::
	//SameBarePool) — while IsBareVisible treats an ownerless context as
	//defensively invisible (it never passes the bare filter).
	static constexpr uint32_t NO_OWNER = 0xFFFFFFFFu;

	//--- per-TU import gates ---
	struct ImportGate
	{
		//Exact module paths (project TUs + external .nmod names).
		std::vector<std::string> exact;
		//Wildcard prefixes ("utils." — recursive, D5).
		std::vector<std::string> wildcards;
		//Builtin namespaces ("io" / "math" / "fs").
		std::vector<std::string> builtins;
	};

	//Register a TU; moduleIndex == position in registration order.
	//Returns false after filling outErrors on a reserved path segment
	//(io/math/fs) — the caller logs and aborts the build.
	bool RegisterUnit(uint32_t moduleIndex, const TranslationUnit& tu,
		const std::string& projectDir,
		std::vector<std::string>& outErrors);

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
		std::vector<std::string>& outErrors);

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

	//NO_OWNER context (no tagged ancestor) never passes a gate: both
	//return false for moduleIndex == NO_OWNER — the gate queries are
	//NO_OWNER-safe (no m_modules[NO_OWNER] indexing).
	bool IsModuleImported(uint32_t moduleIndex,
		const std::string& dottedPath) const;
	bool IsBuiltinImported(uint32_t moduleIndex,
		const std::string& ns) const;
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
	//Functions of a module matching calleeName: project module →
	//scan the global root's top-level members for same-name
	//NK_Functions owned by that module; external module → the
	//same-name subset of its stub table.
	std::vector<SnFunction*> ModuleFunctions(
		const std::string& path,
		const std::string& calleeName) const;

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

	struct ModuleEntry
	{
		std::string path;      //"utils.helper" / "lib"
		bool isExternal = false;
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
