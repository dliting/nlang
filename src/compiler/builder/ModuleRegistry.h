/*---
ModuleRegistry.h - compile-time module registry of the NLang compiler.

Maps every translation unit to its dotted module path, tracks which
module owns each merged top-level symbol, and will hold each unit's
import gate (module import visibility plan, spec §6).
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
class TranslationUnit;
struct ImportSpec;

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
	static constexpr uint32_t NO_OWNER = 0xFFFFFFFFu;

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

	//Owner side table: tag a merged top-level member (also used for
	//namespace members one+ levels deep — namespaces can span TUs).
	void TagOwner(SnField& member, uint32_t moduleIndex);
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
	struct ModuleEntry
	{
		std::string path;      //"utils.helper" / "lib"
		bool isExternal = false;
	};
	std::vector<ModuleEntry> m_modules;
	std::unordered_map<const SnField*, uint32_t> m_ownerOf;
};

} //namespace nlang
