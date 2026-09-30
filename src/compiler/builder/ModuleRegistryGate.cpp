/*---
ModuleRegistryGate.cpp - import gate construction for ModuleRegistry.

Resolves each TU's ImportSpec list into its ImportGate (priority and
wildcard semantics, spec §5-6). Split from ModuleRegistry.cpp to keep
that file within the source-size limit; the registry state and the gate
query helpers still live there.
---*/
#include "ModuleRegistry.h"
#include "TranslationUnit.h"
#include <algorithm>
#include <cassert>

namespace nlang
{

namespace {

bool ContainsValue(const std::vector<std::string>& list,
	const std::string& value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

} //namespace

//Wildcard arm of ApplyImportSpec (D11 union semantics). Evaluated
//BEFORE IsProjectModule in the caller so a root-level namesake
//("utils.n" beside "utils/helper.n") cannot silently degrade the
//wildcard to exact-only and strand the subtree outside the gate.
void ModuleRegistry::ApplyWildcardImport(const std::string& name,
	ImportGate& gate, std::vector<std::string>& outErrors) const
{
	//Recursive prefix over PROJECT module paths (D5): external names
	//are single-segment, so a wildcard never reaches one (§3.3). The
	//importing TU's own path counts toward the match surface.
	const std::string prefix = name + '.';
	const bool exactExists = IsProjectModule(name);
	bool prefixMatched = false;
	for (const ModuleEntry& entry : m_modules)
	{
		if (!entry.isExternal && entry.path.rfind(prefix, 0) == 0)
		{
			prefixMatched = true;
			break;
		}
	}
	//D10: an empty union (no exact module and no prefix match) is
	//almost certainly a typo, not a silent no-op.
	if (!exactExists && !prefixMatched)
	{
		outErrors.push_back("No project modules matched import '"
			+ name + ".*'. Check the project Sources list.");
		return;
	}
	if (exactExists && !ContainsValue(gate.exact, name))
		gate.exact.push_back(name);
	if (prefixMatched && !ContainsValue(gate.wildcards, prefix))
		gate.wildcards.push_back(prefix);
}

//Non-wildcard arm of ApplyImportSpec: compiled-in module, signature-only
//library fallback, external .nmod candidate, or an unresolvable dotted path.
void ModuleRegistry::ApplyNonWildcardImport(const std::string& name,
	ImportGate& gate, std::vector<std::string>& externalOut,
	std::vector<std::string>& outErrors,
	const LibraryNamespacePredicate& isLibraryNamespace) const
{
	//A TU compiled into this build (a project module or an inlined library
	//TU) opens one exact gate — libraries and project modules share it.
	if (IsCompiledInModule(name))
	{
		if (!ContainsValue(gate.exact, name))
			gate.exact.push_back(name);
		return;
	}
	//A library package known to the index but not inlined this build —
	//its .n source was not found in the effective library dirs (a
	//search-path problem). Report it instead of silently gating a package
	//whose call path no longer exists (the legacy builtin codegen was removed).
	if (isLibraryNamespace(name))
	{
		outErrors.push_back("Library package '" + name
			+ "' is indexed but its source was not found in the library "
			"search path; check the configured search paths.");
		return;
	}
	//External .nmod names are single-segment: record the candidate for the
	//loader and open the gate optimistically.
	if (name.find('.') == std::string::npos)
	{
		if (!ContainsValue(gate.exact, name))
			gate.exact.push_back(name);
		if (!ContainsValue(externalOut, name))
			externalOut.push_back(name);
		return;
	}
	//A dotted path that is no project module can never resolve (external
	//names are single-segment).
	outErrors.push_back(ModuleNotFoundText(name));
}

//BuildGate's per-spec arm: §5.1 priority (builtin → project module →
//external .nmod), delegating the wildcard union to ApplyWildcardImport.
void ModuleRegistry::ApplyImportSpec(const ImportSpec& spec,
	ImportGate& gate, std::vector<std::string>& externalOut,
	std::vector<std::string>& outErrors,
	const LibraryNamespacePredicate& isLibraryNamespace) const
{
	const std::string name = spec.DottedName();
	//A wildcard on a library package is rejected BEFORE the library
	//branch — packages are not module trees, and a silently eaten '*'
	//would teach the wrong model.
	if (spec.wildcard && isLibraryNamespace(name))
	{
		outErrors.push_back("Wildcard import cannot target library "
			"package '" + name + "'. Use 'import " + name + ";'.");
		return;
	}
	if (spec.wildcard)
	{
		ApplyWildcardImport(name, gate, outErrors);
		return;
	}
	ApplyNonWildcardImport(name, gate, externalOut, outErrors,
		isLibraryNamespace);
}

bool ModuleRegistry::BuildGate(uint32_t moduleIndex,
	const std::vector<ImportSpec>& specs,
	std::vector<std::string>& externalOut,
	std::vector<std::string>& outErrors,
	const LibraryNamespacePredicate& isLibraryNamespace)
{
	assert(moduleIndex < m_modules.size()
		&& !m_modules[moduleIndex].isExternal);

	ImportGate gate;
	//D7 same-directory auto-visibility applies to PROJECT TUs only. A
	//library TU sees just what it explicitly imports — never its consumer
	//(the TU's own directory is otherwise implicitly imported).
	if (!m_modules[moduleIndex].isLibrary)
	{
		const std::string ownDir = DirectoryOf(moduleIndex);
		for (uint32_t i = 0; i < m_modules.size(); ++i)
		{
			//Library TUs never share same-directory visibility (they are
			//reached only via an explicit import of their namespace).
			if (i == moduleIndex || m_modules[i].isExternal
				|| m_modules[i].isLibrary)
				continue;
			if (DirectoryOf(i) == ownDir
				&& !ContainsValue(gate.exact, m_modules[i].path))
				gate.exact.push_back(m_modules[i].path);
		}
	}

	for (const ImportSpec& spec : specs)
		ApplyImportSpec(spec, gate, externalOut, outErrors,
			isLibraryNamespace);

	if (!outErrors.empty())
		return false;
	m_modules[moduleIndex].gate = std::move(gate);
	return true;
}

} //namespace nlang
