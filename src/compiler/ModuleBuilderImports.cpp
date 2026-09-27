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
#include "VmBackend.h"
#include <fstream>
#include <string>
#include <vector>

static const char *BAR_STR =
	"--------------------------------------------------------------------";

namespace nlang
{
bool ModuleBuilder::LoadImports()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Loading the import modules ...");
	}

	//Load third-party library sources (<name>.n) into the library index
	//before the gates are built, so a discovered namespace opens the same
	//gate as the standard library.
	DiscoverLibrarySources();

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

//Discover third-party library sources. For every single-segment,
//non-wildcard import that is not already a known library namespace,
//search the import dirs for <name>.n and load it once into the library
//index. Only declarations inside a namespace block are indexed, so a
//program file that happens to sit on the import path contributes nothing.
void ModuleBuilder::DiscoverLibrarySources()
{
	for (auto pTransUnit : *m_upTransUnits)
	{
		for (const auto &spec : pTransUnit->Imports())
		{
			if (spec.wildcard)
				continue;
			const std::string name = spec.DottedName();
			//A package file on the import path is single-segment; a dotted
			//name resolves as a project module instead.
			if (name.find('.') != std::string::npos
				|| m_upEnv->IsLibraryNamespace(name))
				continue;
			for (const auto &dir : m_upEnv->Params().m_ImportDirs)
			{
				const std::string path = dir + "/" + name + ".n";
				std::ifstream test(path, std::ios::binary);
				if (test.good())
				{
					m_upEnv->LoadLibrarySource(path);
					break;
				}
			}
		}
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
	for (const auto &dir : m_upEnv->Params().m_ImportDirs)
	{
		std::string path = dir + "/" + name + ".nmod";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return path;
	}
	return std::string();
}
} //namespace nlang
