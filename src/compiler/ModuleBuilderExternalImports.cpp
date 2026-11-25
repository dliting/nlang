/*-----------------------------------------------------------------------------
	ModuleBuilderExternalImports.cpp
	The external-artifact arm of the import pipeline: load an imported
	.ncu/.npkg image, build and register its stub nodes, and locate a
	module's artifact on the library search path. Split from
	ModuleBuilderImports.cpp (2026-11-19 maintainability refactor, zero
	behavior change).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "ModuleBuilderImports.h"
#include <nlang/core/ModuleLoader.h>
#include "builder/CompiledModuleNodeBuilder.hpp"
#include "builder/ModuleRegistry.h"
#include "nlang/core/CompiledModule.h"
#include "nlang/core/NcuPackage.h"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang
{

//Package form (`.npkg`): extract the embedded compile unit. Importable
//packages currently expose exactly ONE member — the entry-recorded one
//for program packages, the only member for library packages. A
//multi-member package without an entry record (a project-mode library
//distribution) is REFUSED rather than silently reduced to its
//alphabetically-first member: the consumer would compile against half
//a surface and miss the invisible members until run time. Per-member
//stub creation needs import-gate semantics for dotted external paths
//(external imports are single-segment today); deferred design
//decision, phase6_loader_design.md section 9.
static CompiledModule LoadModuleArtifact(const std::string &path)
{
	//Package form: extract the embedded compile unit (the entry member,
	//or the single member of a library package) and parse from memory.
	if (std::filesystem::path(path).extension() == NPKG_EXTENSION)
	{
		NcuPackageReader pkg;
		std::string pkgError;
		if (!pkg.Open(path, &pkgError))
			throw std::runtime_error(pkgError);
		std::string memberPath;
		if (const NcuEntryRecord *entry = pkg.EntryRecord())
			memberPath = entry->modulePath;
		else if (pkg.MemberPaths().size() == 1)
			memberPath = pkg.MemberPaths().front();
		else
			throw std::runtime_error(
				"package '" + path + "' carries "
				+ std::to_string(pkg.MemberPaths().size())
				+ " members and no entry record; importable packages "
				"currently expose exactly one module");
		std::string memberBytes;
		if (!pkg.ExtractMember(memberPath, &memberBytes, &pkgError))
			throw std::runtime_error(pkgError);
		return ModuleLoader::LoadFromBytes(path, memberBytes);
	}
	return ModuleLoader::Load(path);
}

//Load one external .ncu candidate end to end: locate, parse, create
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
		cm = LoadModuleArtifact(path);
	}
	catch (const std::exception &e)
	{
		m_upEnv->Log(CLL_Error, "Failed to load module '%s': %s",
			name.c_str(), e.what());
		return false;
	}

	CompiledModuleNodeBuilder builder(TheAST(), name, &m_upEnv->Registry());
	try
	{
		builder.BuildFromCompiledModule(cm);
	}
	catch (const std::exception &e)
	{
		m_upEnv->Log(CLL_Error, "%s", e.what());
		return false;
	}

	RegisterExternalStubs(builder, name);

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
		m_upEnv->Log(CLL_Info, "Loaded module '%s' from %s",
			name.c_str(), path.c_str());

	m_loadedImports.push_back(std::move(cm));
	return true;
}

void ModuleBuilder::RegisterExternalStubs(
	CompiledModuleNodeBuilder &builder, const std::string &name)
{
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
	for (SnFunction* stub : builder.ImportedFunctions())
	{
		stubs.push_back(stub);
		reg.TagOwner(*stub, extIdx);
	}
	reg.SetExternalStubs(extIdx, std::move(stubs));

	//Type stubs (struct/class) are root members, never detached — tag
	//them with the same external index so FindModuleType's owner-
	//filtered root scan binds "<pkg>.<Type>" for qualified type
	//references synthesized from serialized signatures.
	for (SnField* typeStub : builder.ImportedTypes())
		reg.TagOwner(*typeStub, extIdx);

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
