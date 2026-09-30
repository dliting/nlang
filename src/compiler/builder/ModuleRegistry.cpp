/*---
ModuleRegistry.cpp - compile-time module registry of the NLang compiler.
---*/
#include "ModuleRegistry.h"
#include "TranslationUnit.h"
#include "SyntaxTree.h"
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

//Module path = the source path made relative to the project root and
//dotted ("utils/helper.n" -> "utils.helper"). Outside the root (or no
//root at all) the path degenerates to the file stem.
std::string DeriveModulePath(const std::filesystem::path &filePath,
	const std::string &projectDir)
{
	std::string modulePath;
	if (!projectDir.empty())
	{
		std::error_code fsError;
		const std::filesystem::path projectRoot =
			std::filesystem::path(projectDir).lexically_normal();
		const std::filesystem::path relativePath = std::filesystem::relative(
			filePath, projectRoot, fsError);
		if (!fsError && !relativePath.empty() && !EscapesBaseDir(relativePath))
			modulePath = DotifyModulePath(relativePath);
	}
	if (modulePath.empty())
		modulePath = filePath.stem().string();
	return modulePath;
}

//Reserved-name gate (spec §5.2): every dotted segment must avoid the
//built-in namespace names — a project directory named "io" would
//otherwise make its modules unreachable through `import io.*;`
//forever. Returns the first colliding segment, or an empty string.
std::string FindReservedSegment(const std::string &modulePath)
{
	size_t searchFrom = 0;
	for (;;)
	{
		const size_t dotPos = modulePath.find('.', searchFrom);
		const std::string segment = modulePath.substr(searchFrom,
			dotPos == std::string::npos ? std::string::npos
				: dotPos - searchFrom);
		if (IsReservedLibraryName(segment))
			return segment;
		if (dotPos == std::string::npos)
			return std::string();
		searchFrom = dotPos + 1;
	}
}

} //namespace

bool IsReservedLibraryName(const std::string& name)
{
	//io/math/fs each own a stdlib/<ns>.n source and an nlang_<ns>.dll.
	static const char* const kReservedLibraryNames[] = { "io", "math", "fs" };
	for (const char* reserved : kReservedLibraryNames)
		if (name == reserved)
			return true;
	return false;
}

std::string ModuleNotFoundText(const std::string& moduleName)
{
	return "Module '" + moduleName +
		"' not found. Check the project Sources list or -I import "
		"path.";
}

bool ModuleRegistry::RegisterUnit(uint32_t moduleIndex,
	const TranslationUnit& tu, const std::string& projectDir,
	std::vector<std::string>& outErrors, bool isLibrary)
{
	//Registration order is the module index space shared with
	//AddExternalModule; a drift here would silently mis-own symbols.
	assert(moduleIndex == m_modules.size());

	const std::filesystem::path filePath =
		std::filesystem::path(tu.FilePath()).lexically_normal();

	//A stem containing a dot ("my.lib.n") would register as module path
	//"my.lib" while DirectoryOf reports "my" — the same-directory set and
	//the `import my.*;` wildcard surface would both silently mis-include
	//it. Dots in a module name mean directories, so the stem must be a
	//single identifier; reject the shape instead of mis-registering it.
	const std::string stem = filePath.stem().string();
	if (stem.find('.') != std::string::npos)
	{
		outErrors.push_back("Source file name '" + stem +
			"' contains a dot before the '.n' extension. Rename the file:"
			" a dot in a module name stands for a directory.");
		return false;
	}

	const std::string modulePath = DeriveModulePath(filePath, projectDir);
	//A library TU legitimately occupies a reserved library namespace name
	//(the standard library's own io.n); the reserved-segment guard only
	//keeps PROJECT directories from taking such a name.
	if (!isLibrary)
	{
		const std::string reservedSegment = FindReservedSegment(modulePath);
		if (!reservedSegment.empty())
		{
			outErrors.push_back("Module path segment '" + reservedSegment +
				"' collides with a built-in namespace.");
			return false;
		}
	}

	ModuleEntry entry;
	entry.path = std::move(modulePath);
	entry.isLibrary = isLibrary;
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

bool ModuleRegistry::IsLibraryModule(uint32_t moduleIndex) const
{
	return moduleIndex < m_modules.size()
		&& m_modules[moduleIndex].isLibrary;
}

bool ModuleRegistry::IsProjectModule(const std::string& dottedPath) const
{
	for (const ModuleEntry& entry : m_modules)
	{
		//Inline library TUs are not project modules: they are reached only
		//through an explicit import, never same-directory/wildcard.
		if (!entry.isExternal && !entry.isLibrary
			&& entry.path == dottedPath)
			return true;
	}
	return false;
}

bool ModuleRegistry::IsCompiledInModule(const std::string& dottedPath) const
{
	//Project modules and inline library TUs alike (no isExternal, no
	//isLibrary filter) — the unified compiled-in surface.
	for (const ModuleEntry& entry : m_modules)
	{
		if (!entry.isExternal && entry.path == dottedPath)
			return true;
	}
	return false;
}

//Import gate construction — ApplyWildcardImport, ApplyNonWildcardImport,
//ApplyImportSpec, BuildGate — lives in ModuleRegistryGate.cpp.

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

bool ModuleRegistry::IsKnownModule(const std::string& dottedPath) const
{
	for (const ModuleEntry& entry : m_modules)
	{
		if (entry.path == dottedPath)
			return true;
	}
	return false;
}

bool ModuleRegistry::HasKnownModuleStartingWith(
	const std::string& dottedPrefix) const
{
	const std::string prefix = dottedPrefix + '.';
	for (const ModuleEntry& entry : m_modules)
	{
		if (entry.path == dottedPrefix
			|| entry.path.rfind(prefix, 0) == 0)
			return true;
	}
	return false;
}

void ModuleRegistry::SetExternalStubs(uint32_t moduleIndex,
	std::vector<SnFunction*> stubs)
{
	//Stubs can only belong to an external entry — same defensive
	//contract as BuildGate's moduleIndex preconditions.
	assert(moduleIndex < m_modules.size()
		&& m_modules[moduleIndex].isExternal);
	m_externalStubs[moduleIndex] = std::move(stubs);
}

std::vector<SnFunction*> ModuleRegistry::CompiledInFunctions(
	uint32_t moduleIndex, const std::string& calleeName) const
{
	std::vector<SnFunction*> owned;
	SnNamespace* pRoot = TheAST().Root();
	if (pRoot == nullptr)
		return owned;
	//Library units and project modules are the same kind of unit now:
	//every member sits on root and carries its owner tag, so the
	//`namespace <path>` container lookup is gone. Identity = PackageOf.
	auto range = pRoot->Members().NameDict().equal_range(calleeName);
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField& member = *iField->second;
		if (member.Kind() == NK_Function && OwnerOf(member) == moduleIndex)
			owned.push_back(static_cast<SnFunction*>(&member));
	}
	return owned;
}

std::vector<SnFunction*> ModuleRegistry::ModuleFunctions(
	const std::string& path, const std::string& calleeName) const
{
	//First path hit returns — a registered name is either a project module
	//or an external .nmod, never both: the import gate resolves project
	//names before the external load (don't "fix" this into a full scan).
	for (uint32_t i = 0; i < m_modules.size(); ++i)
	{
		if (m_modules[i].path != path)
			continue;
		if (m_modules[i].isExternal)
		{
			//Same-name subset of the module's stub table (all overloads).
			const auto iFound = m_externalStubs.find(i);
			if (iFound == m_externalStubs.end())
				return std::vector<SnFunction*>{};
			std::vector<SnFunction*> matching;
			for (SnFunction* pStub : iFound->second)
				if (pStub->Name() == calleeName)
					matching.push_back(pStub);
			return matching;
		}
		return CompiledInFunctions(i, calleeName);
	}
	return std::vector<SnFunction*>{};
}

namespace {

//The source-level declarations a `path.Type` reference can bind.
bool IsBindableTypeDecl(const SnField& member)
{
	switch (member.Kind())
	{
	case NK_ClassDecl:
	case NK_StructDecl:
	case NK_EnumDecl:
	case NK_InterfaceDecl:
		return true;
	default:
		return false;
	}
}

} //namespace

SnField* ModuleRegistry::FindModuleType(const std::string& path,
	const std::string& typeName) const
{
	//Find the registered unit for path (project or inline library).
	//External .nmod stubs carry no source-level type declarations in v1.
	for (uint32_t i = 0; i < m_modules.size(); ++i)
	{
		if (m_modules[i].path != path)
			continue;
		if (m_modules[i].isExternal)
			return nullptr;

		//Every member sits on root (phase 5 removed the shell syntax); the
		//unit's identity is the owner tag, not any container.
		SnNamespace* pRoot = TheAST().Root();
		if (pRoot == nullptr)
			return nullptr;

		//Owner-filtered scan, like the function side: `path.Type` must bind a
		//declaration the unit itself owns, never a same-name type another TU
		//merged into the same container.
		auto range = pRoot->Members().NameDict().equal_range(typeName);
		for (auto iField = range.first; iField != range.second; ++iField)
			if (IsBindableTypeDecl(*iField->second)
				&& OwnerOf(*iField->second) == i)
				return iField->second;
	}
	return nullptr;
}

void ModuleRegistry::TagOwner(SnField& member, uint32_t moduleIndex)
{
	m_ownerOf[&member] = moduleIndex;
}

void ModuleRegistry::EraseOwner(SnField& member)
{
	m_ownerOf.erase(&member);
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

std::string ModuleRegistry::PackageOf(const SnField& member) const
{
	//The unit's dotted path is the package name; an untagged member
	//(built-in type, synthetic generic instantiation) has no package.
	//Deliberately NOT derived from the AST namespace chain: the container
	//exists only where a library file literally writes `namespace <path>`,
	//so the chain cannot express a path-derived package for project units.
	const uint32_t owner = OwnerOf(member);
	if (owner == NO_OWNER || owner >= m_modules.size())
		return std::string();
	return m_modules[owner].path;
}

std::string ModuleRegistry::QualifiedName(const SnField& member) const
{
	const std::string pkg = PackageOf(member);
	if (pkg.empty())
		return member.Name();
	return pkg + "." + member.Name();
}

} //namespace nlang
