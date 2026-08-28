/*---
ModuleRegistry.cpp - compile-time module registry of the NLang compiler.
---*/
#include "ModuleRegistry.h"
#include "TranslationUnit.h"
#include "SyntaxTree.h"
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <cassert>
#include <filesystem>

namespace nlang
{

namespace {

//Directory sentinel for external .nmod entries: no reasonable directory
//name contains '$', so this never equals a TU directory.
const char EXTERNAL_DIR_SENTINEL[] = "$external$";

//"utils/helper.n" relative path -> dotted module path "utils.helper";
//both separator flavors are folded so a mixed-separator source path
//still yields one canonical module path.
std::string DotifyModulePath(std::filesystem::path relativePath)
{
	relativePath.replace_extension();   //drop ".n"
	std::string dotted = relativePath.generic_string();
	std::replace(dotted.begin(), dotted.end(), '\\', '.');
	std::replace(dotted.begin(), dotted.end(), '/', '.');
	return dotted;
}

//True when the computed relative path leaves the base directory
//("../.."-shaped): such a source is not inside the project root.
bool EscapesBaseDir(const std::filesystem::path& relativePath)
{
	return !relativePath.empty() && *relativePath.begin() == "..";
}

bool ContainsValue(const std::vector<std::string>& list,
	const std::string& value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

} //namespace

std::string ModuleNotFoundText(const std::string& moduleName)
{
	return "Module '" + moduleName +
		"' not found. Check the project Sources list or -I import "
		"path.";
}

bool ModuleRegistry::RegisterUnit(uint32_t moduleIndex,
	const TranslationUnit& tu, const std::string& projectDir,
	std::vector<std::string>& outErrors)
{
	//Registration order is the module index space shared with
	//AddExternalModule; a drift here would silently mis-own symbols.
	assert(moduleIndex == m_modules.size());

	std::error_code fsError;
	const std::filesystem::path filePath =
		std::filesystem::path(tu.FilePath()).lexically_normal();

	//Module path = the source path made relative to the project root
	//and dotted ("utils/helper.n" -> "utils.helper"). Outside the root
	//(or no root at all) the path degenerates to the file stem.
	std::string modulePath;
	if (!projectDir.empty())
	{
		const std::filesystem::path projectRoot =
			std::filesystem::path(projectDir).lexically_normal();
		const std::filesystem::path relativePath = std::filesystem::relative(
			filePath, projectRoot, fsError);
		if (!fsError && !relativePath.empty() && !EscapesBaseDir(relativePath))
			modulePath = DotifyModulePath(relativePath);
	}
	if (modulePath.empty())
		modulePath = filePath.stem().string();

	//Reserved-name gate (spec §5.2): every dotted segment must avoid the
	//built-in namespace names — a project directory named "io" would
	//otherwise make its modules unreachable through `import io.*;`
	//forever.
	size_t searchFrom = 0;
	for (;;)
	{
		const size_t dotPos = modulePath.find('.', searchFrom);
		const std::string segment = modulePath.substr(searchFrom,
			dotPos == std::string::npos ? std::string::npos
				: dotPos - searchFrom);
		if (IsStdLibNamespaceName(segment))
		{
			outErrors.push_back("Module path segment '" + segment +
				"' collides with a built-in namespace.");
			return false;
		}
		if (dotPos == std::string::npos)
			break;
		searchFrom = dotPos + 1;
	}

	ModuleEntry entry;
	entry.path = std::move(modulePath);
	m_modules.push_back(std::move(entry));
	return true;
}

void ModuleRegistry::Reset()
{
	m_modules.clear();
	m_ownerOf.clear();
	m_externalStubs.clear();
}

uint32_t ModuleRegistry::AddExternalModule(const std::string& name)
{
	ModuleEntry entry;
	entry.path = name;
	entry.isExternal = true;
	m_modules.push_back(std::move(entry));
	return static_cast<uint32_t>(m_modules.size() - 1);
}

const std::string& ModuleRegistry::ModulePathOf(uint32_t moduleIndex) const
{
	static const std::string s_Empty;
	return moduleIndex < m_modules.size()
		? m_modules[moduleIndex].path : s_Empty;
}

std::string ModuleRegistry::DirectoryOf(uint32_t moduleIndex) const
{
	if (moduleIndex >= m_modules.size())
		return std::string();
	const ModuleEntry& entry = m_modules[moduleIndex];
	if (entry.isExternal)
		return EXTERNAL_DIR_SENTINEL;
	//No '.' means a root file: its directory part is empty.
	const size_t lastDot = entry.path.find_last_of('.');
	return lastDot == std::string::npos
		? std::string() : entry.path.substr(0, lastDot);
}

bool ModuleRegistry::IsExternal(uint32_t moduleIndex) const
{
	return moduleIndex < m_modules.size()
		&& m_modules[moduleIndex].isExternal;
}

bool ModuleRegistry::IsProjectModule(const std::string& dottedPath) const
{
	for (const ModuleEntry& entry : m_modules)
	{
		if (!entry.isExternal && entry.path == dottedPath)
			return true;
	}
	return false;
}

bool ModuleRegistry::BuildGate(uint32_t moduleIndex,
	const std::vector<ImportSpec>& specs,
	std::vector<std::string>& externalOut,
	std::vector<std::string>& outErrors)
{
	assert(moduleIndex < m_modules.size()
		&& !m_modules[moduleIndex].isExternal);

	ImportGate gate;
	//D7: the TU's own directory is implicitly imported — same-dir
	//files resolve bare AND qualified without an explicit import.
	const std::string ownDir = DirectoryOf(moduleIndex);
	for (uint32_t i = 0; i < m_modules.size(); ++i)
	{
		if (i == moduleIndex || m_modules[i].isExternal)
			continue;
		if (DirectoryOf(i) == ownDir
			&& !ContainsValue(gate.exact, m_modules[i].path))
			gate.exact.push_back(m_modules[i].path);
	}

	for (const ImportSpec& spec : specs)
	{
		const std::string name = spec.DottedName();
		//D10: a wildcard on a builtin name is rejected BEFORE the
		//builtin branch — builtins are namespaces, not module trees,
		//and a silently eaten '*' would teach the wrong model.
		if (spec.wildcard && IsStdLibNamespaceName(name))
		{
			outErrors.push_back("Wildcard import cannot target builtin "
				"namespace '" + name + "'. Use 'import " + name + ";'.");
			continue;
		}
		//§5.1 priority: builtin → project module → external .nmod.
		if (IsStdLibNamespaceName(name))
		{
			if (!ContainsValue(gate.builtins, name))
				gate.builtins.push_back(name);
			continue;
		}
		//D11: a wildcard is a UNION — the exact module "X" (when X is a
		//project module) plus every "X."-prefixed project module.
		//Evaluated BEFORE IsProjectModule so a root-level namesake
		//("utils.n" beside "utils/helper.n") cannot silently degrade the
		//wildcard to exact-only and strand the subtree outside the gate.
		if (spec.wildcard)
		{
			//Recursive prefix over PROJECT module paths (D5): external
			//names are single-segment, so a wildcard never reaches one
			//(§3.3). The importing TU's own path counts toward the
			//match surface.
			const std::string prefix = name + '.';
			const bool exactExists = IsProjectModule(name);
			bool prefixMatched = false;
			for (const ModuleEntry& entry : m_modules)
			{
				if (!entry.isExternal
					&& entry.path.rfind(prefix, 0) == 0)
				{
					prefixMatched = true;
					break;
				}
			}
			//D10: an empty union (no exact module and no prefix match)
			//is almost certainly a typo, not a silent no-op.
			if (!exactExists && !prefixMatched)
			{
				outErrors.push_back("No project modules matched import '"
					+ name + ".*'. Check the project Sources list.");
				continue;
			}
			if (exactExists && !ContainsValue(gate.exact, name))
				gate.exact.push_back(name);
			if (prefixMatched && !ContainsValue(gate.wildcards, prefix))
				gate.wildcards.push_back(prefix);
			continue;
		}
		if (IsProjectModule(name))
		{
			if (!ContainsValue(gate.exact, name))
				gate.exact.push_back(name);
			continue;
		}
		//External .nmod names are single-segment: record the candidate
		//for the loader and open the gate optimistically — if the
		//.nmod fails to load, the build aborts right after.
		if (name.find('.') == std::string::npos)
		{
			if (!ContainsValue(gate.exact, name))
				gate.exact.push_back(name);
			if (!ContainsValue(externalOut, name))
				externalOut.push_back(name);
			continue;
		}
		//A dotted path that is no project module can never resolve
		//(external names are single-segment, §5.3 single-file rule).
		outErrors.push_back(ModuleNotFoundText(name));
	}

	if (!outErrors.empty())
		return false;
	m_modules[moduleIndex].gate = std::move(gate);
	return true;
}

bool ModuleRegistry::IsModuleImported(uint32_t moduleIndex,
	const std::string& dottedPath) const
{
	//NO_OWNER context never passes a gate (no m_modules[NO_OWNER]).
	if (moduleIndex >= m_modules.size())
		return false;
	const ImportGate& gate = m_modules[moduleIndex].gate;
	if (ContainsValue(gate.exact, dottedPath))
		return true;
	for (const std::string& prefix : gate.wildcards)
	{
		if (dottedPath.rfind(prefix, 0) == 0)
			return true;
	}
	return false;
}

bool ModuleRegistry::IsBuiltinImported(uint32_t moduleIndex,
	const std::string& ns) const
{
	if (moduleIndex >= m_modules.size())
		return false;
	const ImportGate& gate = m_modules[moduleIndex].gate;
	return ContainsValue(gate.builtins, ns);
}

bool ModuleRegistry::IsKnownModule(const std::string& dottedPath) const
{
	for (const ModuleEntry& entry : m_modules)
	{
		if (entry.path == dottedPath)
			return true;
	}
	return false;
}

void ModuleRegistry::SetExternalStubs(uint32_t moduleIndex,
	std::vector<SnFunction*> stubs)
{
	m_externalStubs[moduleIndex] = std::move(stubs);
}

std::vector<SnFunction*> ModuleRegistry::ModuleFunctions(
	const std::string& path, const std::string& calleeName) const
{
	for (uint32_t i = 0; i < m_modules.size(); ++i)
	{
		if (m_modules[i].path != path)
			continue;
		if (m_modules[i].isExternal)
		{
			//Same-name subset of the module's stub table — symmetric
			//with the project branch (all overloads of the name).
			const auto iFound = m_externalStubs.find(i);
			if (iFound == m_externalStubs.end())
				return std::vector<SnFunction*>{};
			std::vector<SnFunction*> matching;
			for (SnFunction* pStub : iFound->second)
			{
				if (pStub->Name() == calleeName)
					matching.push_back(pStub);
			}
			return matching;
		}
		//Project module: same-name functions owned by this module among
		//the merged root's top-level members (owner tags land in
		//MergeTransUnits). Namespaces are out of the v1 surface.
		std::vector<SnFunction*> owned;
		SnNamespace* pRoot = TheAST().Root();
		if (pRoot == nullptr)
			return owned;
		for (SnField& member : pRoot->Members())
		{
			if (member.Kind() == NK_Function
				&& member.Name() == calleeName
				&& OwnerOf(member) == i)
				owned.push_back(static_cast<SnFunction*>(&member));
		}
		return owned;
	}
	return std::vector<SnFunction*>{};
}

void ModuleRegistry::TagOwner(SnField& member, uint32_t moduleIndex)
{
	m_ownerOf[&member] = moduleIndex;
}

uint32_t ModuleRegistry::OwnerOf(const SnField& member) const
{
	const auto iFound = m_ownerOf.find(&member);
	return iFound == m_ownerOf.end() ? NO_OWNER : iFound->second;
}

uint32_t ModuleRegistry::OwnerOfContext(const SyntaxNode& context) const
{
	for (const SyntaxNode* pNode = &context; pNode != nullptr;
		pNode = pNode->Parent())
	{
		//Only SnField ancestors (functions / namespaces / classes) can
		//carry an owner tag; other chain nodes just don't match.
		const SnField* pField = dynamic_cast<const SnField*>(pNode);
		if (pField != nullptr)
		{
			const auto iFound = m_ownerOf.find(pField);
			if (iFound != m_ownerOf.end())
				return iFound->second;
		}
	}
	return NO_OWNER;
}

} //namespace nlang
