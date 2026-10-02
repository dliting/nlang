/*-----------------------------------------------------------------------------
	ModuleBuilderImports.cpp
	The .ncu import pipeline of ModuleBuilder: per-TU gates, external
	module loading and stub registration. Split from ModuleBuilder.cpp
	(2026-09-27 maintainability refactor, zero behavior change).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "SyntaxTree.h"
#include "TranslationUnit.h"
#include "builder/ModuleRegistry.h"
#include "builder/CompiledModuleNodeBuilder.hpp"
#include "ModuleLoader.h"
#include "nlang/vm/NcuPackage.h"
#include "ScriptParser.h"
#include "VmBackend.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static const char *BAR_STR =
	"--------------------------------------------------------------------";

namespace nlang
{
namespace
{
//Effective library search directories: explicit/configured import dirs
//first, then the stdlib dir, so the standard library is discoverable even
//when a direct BuildParams caller did not list it (the tool layer normally
//prepends it through BuildLibrarySearchPath).
std::vector<std::string> EffectiveLibraryDirs(const BuildParams &params)
{
	std::vector<std::string> dirs;
	dirs.assign(params.m_ImportDirs.begin(), params.m_ImportDirs.end());
	if (!params.m_sStdLibDir.empty()
		&& std::find(dirs.begin(), dirs.end(), params.m_sStdLibDir)
			== dirs.end())
		dirs.push_back(params.m_sStdLibDir);
	return dirs;
}
} //namespace

bool ModuleBuilder::LoadImports()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Loading the import modules ...");
	}

	//Library <name>.n sources were already discovered and fully parsed
	//(DiscoverLibraryUnits, before registration); here only per-TU gates
	//and .ncu external modules remain.
	std::vector<std::string> externalNames;
	if (!BuildImportGates(externalNames))
		return false;

	//Load the external .ncu candidates once each (LOADING is global;
	//VISIBILITY stays per-TU via the gates built above).
	for (const auto &name : externalNames)
	{
		if (!LoadExternalModule(name))
			return false;
	}

	return true;
}

//Per-TU gates (D1: imports are file-scoped — one file's import must
//not leak visibility to other files). Builtin / project names land
//in the gate; single-segment non-project names are external .ncu
//candidates collected below. PtrList is a std::list (no operator[]),
//so the module index travels with a local counter.
bool ModuleBuilder::BuildImportGates(
	std::vector<std::string> &rExternalNames)
{
	ModuleRegistry &reg = m_upEnv->Registry();
	std::vector<std::string> gateErrors;
	uint32_t gateModuleIndex = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		if (!reg.BuildGate(gateModuleIndex++, pTransUnit->Imports(),
				rExternalNames, gateErrors,
				[this](const std::string &ns) {
					return m_upEnv->IsLibraryPackage(ns);
				}))
		{
			for (const auto &error : gateErrors)
				m_upEnv->Log(CLL_Error, "%s", error.c_str());
			return false;
		}
	}
	return true;
}

//Every (file, root) match across the effective library dirs: the
//duplicate-package diagnosis must see the second root too, so the scan
//does not stop at the first hit.
std::vector<std::pair<std::string, std::string>>
ModuleBuilder::FindLibrarySourceMatches(const std::string& dottedName) const
{
	std::string rel = dottedName;
	std::replace(rel.begin(), rel.end(), '.', '/');
	std::vector<std::pair<std::string, std::string>> matches;
	for (const auto &dir : EffectiveLibraryDirs(m_upEnv->Params()))
	{
		const std::string path = dir + "/" + rel + ".n";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			matches.push_back({ path, dir });
	}
	return matches;
}

//Locate a library source for a dotted package name: `a.b.c` is
//<root>/a/b/c.n on the first matching search dir. Returns the file and the
//root it was found under, because the package name is the path RELATIVE TO
//THAT ROOT — a bare stem would make vendor/graphics.n register as `graphics`
//and be unreachable as `import vendor.graphics;`.
std::pair<std::string, std::string> ModuleBuilder::FindLibrarySourceFile(
	const std::string& dottedName) const
{
	const auto matches = FindLibrarySourceMatches(dottedName);
	if (matches.empty())
		return { };
	return matches.front();
}

//Index the signatures of one library source file (so its package opens
//the same gate as the standard library) and then parse it fully as an
//inline library translation unit. Each absolute path is inlined once.
//packageRoot is the search root the file was found under — the package
//name is the path relative to THAT root (Step 4's RegisterUnits reads it
//back through m_librarySourceRoots).
bool ModuleBuilder::ParseLibraryUnit(const std::string &path,
	const std::string &packageRoot)
{
	std::error_code fsError;
	const std::string absPath =
		std::filesystem::absolute(std::filesystem::path(path), fsError)
			.lexically_normal().string();
	if (!m_inlinedLibraryFiles.insert(absPath).second)
		return false;  //already fully parsed this build
	m_librarySourceRoots[absPath] = packageRoot;

	//Signature index (idempotent): declarations are indexed under the
	//matched-root package, so the package resolves as a library package
	//in gates.
	m_upEnv->LoadLibrarySource(path, packageRoot);

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
		m_upEnv->Log(CLL_Info, "Parsing library %s ...", path.c_str());

	TranslationUnit *pUnit = new TranslationUnit(path);
	m_upTransUnits->push_back(pUnit);
	ScriptParser parser(*m_upEnv);
	parser.ParseUnit(*pUnit, m_upEnv->ContainFlags(MBF_ParserDebug));
	return true;
}

//One import's library-source handling (DiscoverLibraryUnits' inner loop
//body): a dotted name maps to <root>/a/b/c.n under the matched root (a
//bare stem is the same rule at one segment). More than one root offering
//the same package is the duplicate-package error — the reserved-name
//table's replacement — naming both source paths instead of silently
//binding the import to whichever root searched first. True when a TU
//was added (drives the fixed-point iteration).
bool ModuleBuilder::HandleLibraryImport(const std::string &dottedName)
{
	const auto matches = FindLibrarySourceMatches(dottedName);
	if (matches.size() > 1)
	{
		std::string message = "Duplicate package '" + dottedName
			+ "': '" + matches.front().first + "' and '"
			+ matches.back().first + "'.";
		m_upEnv->Log(CLL_Error, "%s", message.c_str());
		return false;
	}
	if (matches.empty())
		return false;
	//A match that IS a project source is already compiled in; inlining it
	//again would register the same package twice.
	std::error_code fsError;
	const std::string absPath =
		std::filesystem::absolute(std::filesystem::path(
			matches.front().first), fsError).lexically_normal().string();
	if (m_projectSourceFiles.count(absPath) > 0)
		return false;
	return ParseLibraryUnit(matches.front().first, matches.front().second);
}

//Discover every import-reachable library <pkg>.n source. Iterate the
//imports of all known TUs to a fixed point so a library can depend on a
//library: each pass may add library TUs whose imports are scanned on the
//next pass. Non-wildcard imports are candidates (a dotted name maps to
//<root>/a/b/c.n under the matched root; a bare stem is the same rule at
//one segment), whether or not the signature index already knows the
//package, so the standard library is inlined exactly like a third-party
//source library.
void ModuleBuilder::DiscoverLibraryUnits()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Discovering library sources ...");
	}

	size_t scanned = 0;  //TUs already inspected (TUs only ever append)
	bool changed = true;
	while (changed)
	{
		changed = false;
		//Only inspect the TUs present when this pass began: a library
		//added this pass is picked up on the next pass (the seen counter
		//bounds the scan regardless of list-end iterator stability).
		const size_t total = m_upTransUnits->size();
		size_t seen = 0;
		for (auto it = m_upTransUnits->begin();
			it != m_upTransUnits->end() && seen < total; ++it, ++seen)
		{
			if (seen < scanned)
				continue;
			const TranslationUnit *pUnit = *it;
			for (const ImportSpec &spec : pUnit->Imports())
			{
				if (spec.wildcard)
					continue;
				if (HandleLibraryImport(spec.DottedName()))
					changed = true;
			}
		}
		scanned = total;
	}
}

//Load one external .ncu candidate end to end: locate, parse, mint
//stubs, register owners, keep detached stubs alive, then take ownership
//of the compiled module.
//Package form (`.npkg`): extract the embedded compile unit (entry
//member, or the first member) and parse from memory.
static CompiledModule LoadModuleArtifact(const std::string &path)
{
	//Package form: extract the embedded compile unit (the entry member,
	//or the first member) and parse from memory.
	if (std::filesystem::path(path).extension() == NPKG_EXTENSION)
	{
		NcuPackageReader pkg;
		std::string pkgError;
		if (!pkg.Open(path, &pkgError))
			throw std::runtime_error(pkgError);
		std::string memberPath;
		if (const NcuEntryRecord *entry = pkg.EntryRecord())
			memberPath = entry->modulePath;
		else if (!pkg.MemberPaths().empty())
			memberPath = pkg.MemberPaths().front();
		std::string memberBytes;
		if (!pkg.ExtractMember(memberPath, &memberBytes, &pkgError))
			throw std::runtime_error(pkgError);
		return ModuleLoader::LoadFromBytes(path, memberBytes);
	}
	return ModuleLoader::Load(path);
}

bool ModuleBuilder::LoadExternalModule(const std::string &name)
{
	std::string path = FindModuleFile(name);
	if (path.empty())
	{
		const std::string notFound = ModuleNotFoundText(name);
		m_upEnv->Log(CLL_Error, "%s", notFound.c_str());
		return false;
	}

	CompiledModule cm;
	try
	{
		cm = LoadModuleArtifact(path);
	}
	catch (const std::exception &e)
	{
		m_upEnv->Log(CLL_Error, "Failed to load module '%s': %s",
			name.c_str(), e.what());
		return false;
	}

	//srcModIdx = current length of m_loadedImports (before push), which
	//matches the index this module will occupy after the push below.
	uint32_t srcModIdx = static_cast<uint32_t>(m_loadedImports.size());

	CompiledModuleNodeBuilder builder(TheAST(), srcModIdx, name);
	try
	{
		builder.BuildFromCompiledModule(cm);
	}
	catch (const std::exception &e)
	{
		m_upEnv->Log(CLL_Error, "%s", e.what());
		return false;
	}

	RegisterExternalStubs(builder, srcModIdx, name);

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
		m_upEnv->Log(CLL_Info, "Loaded module '%s' from %s",
			name.c_str(), path.c_str());

	m_loadedImports.push_back(std::move(cm));
	return true;
}

void ModuleBuilder::RegisterExternalStubs(
	CompiledModuleNodeBuilder &builder, uint32_t srcModIdx,
	const std::string &name)
{
	//Register stub→source-index entries into VmBackend side-table.
	//Defer if backend isn't ready yet — but in current flow, CreateModule
	//runs before LoadImports and sets up the backend, so it's available.
	if (auto *backend = m_upEnv->Backend())
	{
		if (auto *vmBackend = dynamic_cast<VmBackend*>(backend))
		{
			for (const auto &entry : builder.ImportedFunctions())
				vmBackend->RegisterImportedFunctionStub(entry.stub,
					srcModIdx, entry.srcFuncIdx);
		}
	}

	//Import visibility (C1): the module joins the registry as an
	//EXTERNAL entry owning every free-function stub it contributed,
	//tagged at the same point the stubs join the root — before
	//MergeTransUnits tags the TU members. Class methods are not free
	//functions (they stay inside their class stub), so this table is
	//the whole qualified-call surface for v1.
	ModuleRegistry &reg = m_upEnv->Registry();
	uint32_t extIdx = reg.AddExternalModule(name);
	std::vector<SnFunction*> stubs;
	stubs.reserve(builder.ImportedFunctions().size());
	for (const auto &entry : builder.ImportedFunctions())
	{
		stubs.push_back(entry.stub);
		reg.TagOwner(*entry.stub, extIdx);
	}
	reg.SetExternalStubs(extIdx, std::move(stubs));

	//Detached stubs (names already in root) joined the tables above
	//but not the root — keep them alive for the duration of the build.
	for (auto &upStub : builder.TakeDetachedStubs())
		m_upDetachedImportStubs.push_back(std::move(upStub));
}

std::string ModuleBuilder::FindModuleFile(const std::string &name) const
{
	for (const auto &dir : EffectiveLibraryDirs(m_upEnv->Params()))
	{
			std::string path = dir + "/" + name + NCU_EXTENSION;
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return path;
		//Package form: <name>.npkg carries the compiled unit embedded.
		path = dir + "/" + name + NPKG_EXTENSION;
		test.clear();
		test.open(path, std::ios::binary);
		if (test.good())
			return path;
	}
	return std::string();
}
} //namespace nlang
