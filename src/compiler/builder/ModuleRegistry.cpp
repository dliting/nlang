/*---
ModuleRegistry.cpp - compile-time module registry of the NLang compiler.
---*/
#include "ModuleRegistry.h"
#include "TranslationUnit.h"
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

} //namespace

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
