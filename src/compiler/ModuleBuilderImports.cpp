/*-----------------------------------------------------------------------------
	ModuleBuilderImports.cpp
	The .nmod import pipeline of ModuleBuilder: per-TU gates, external
	module loading and stub registration. Split from ModuleBuilder.cpp
	(2026-09-27 maintainability refactor, zero behavior change).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "SyntaxTree.h"
#include "TranslationUnit.h"
#include "builder/ModuleRegistry.h"
#include "builder/CompiledModuleNodeBuilder.hpp"
#include "ModuleLoader.h"
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
	//and .nmod external modules remain.
	std::vector<std::string> externalNames;
	if (!BuildImportGates(externalNames))
		return false;

	//Load the external .nmod candidates once each (LOADING is global;
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
//in the gate; single-segment non-project names are external .nmod
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
					return m_upEnv->IsLibraryNamespace(ns);
				}))
		{
			for (const auto &error : gateErrors)
				m_upEnv->Log(CLL_Error, "%s", error.c_str());
			return false;
		}
	}
	return true;
}

//Locate <name>.n on the effective library dirs; the first match, or empty.
//A package file is single-segment, so callers pass a bare name.
std::string ModuleBuilder::FindLibrarySourceFile(
	const std::string &name) const
{
	for (const auto &dir : EffectiveLibraryDirs(m_upEnv->Params()))
	{
		const std::string path = dir + "/" + name + ".n";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return path;
	}
	return std::string();
}

//Index the signatures of one library source file (so its namespace opens
//the same gate as the standard library) and then parse it fully as an
//inline library translation unit. Each absolute path is inlined once.
bool ModuleBuilder::ParseLibraryUnit(const std::string &path)
{
	std::error_code fsError;
	const std::string absPath =
		std::filesystem::absolute(std::filesystem::path(path), fsError)
			.lexically_normal().string();
	if (!m_inlinedLibraryFiles.insert(absPath).second)
		return false;  //already fully parsed this build

	//Signature index (idempotent): declarations inside the namespace are
	//indexed, so the namespace resolves as a library namespace in gates.
	m_upEnv->LoadLibrarySource(path);

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
		m_upEnv->Log(CLL_Info, "Parsing library %s ...", path.c_str());

	TranslationUnit *pUnit = new TranslationUnit(path);
	m_upTransUnits->push_back(pUnit);
	ScriptParser parser(*m_upEnv);
	parser.ParseUnit(*pUnit, m_upEnv->ContainFlags(MBF_ParserDebug));
	return true;
}

//Discover every import-reachable library <name>.n source. Iterate the
//imports of all known TUs to a fixed point so a library can depend on a
//library: each pass may add library TUs whose imports are scanned on the
//next pass. Only single-segment, non-wildcard imports that are not yet a
//known (signature-indexed) library namespace are candidates; dotted
//names resolve as project modules instead.
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
				const std::string name = spec.DottedName();
				//Dotted names resolve as project modules, not package files.
				//Single-segment names are looked up whether or not the
				//signature index already knows them, so the standard library
				//is inlined exactly like a third-party source library.
				if (name.find('.') != std::string::npos)
					continue;
				const std::string path = FindLibrarySourceFile(name);
				if (!path.empty() && ParseLibraryUnit(path))
					changed = true;
			}
		}
		scanned = total;
	}
}

//Load one external .nmod candidate end to end: locate, parse, mint
//stubs, register owners, keep detached stubs alive, then take ownership
//of the compiled module.
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
		cm = ModuleLoader::Load(path);
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
		std::string path = dir + "/" + name + ".nmod";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return path;
	}
	return std::string();
}
} //namespace nlang
