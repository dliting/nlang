/*-----------------------------------------------------------------------------
	ModuleBuilderExternalImports.cpp
	The external-artifact arm of the import pipeline: the discovery-phase
	lookup of an import name's artifact (a unit file or a package member on
	the library search path), and the load-phase registration of the found
	units — their stub nodes, registry entries and stub tables. Split from
	ModuleBuilderImports.cpp (2026-11-19 maintainability refactor).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "ModuleBuilderImports.h"
#include "ModuleLoader.h"
#include "builder/CompiledModuleNodeBuilder.hpp"
#include "builder/ModuleRegistry.h"
#include "nlang/vm/NcuPackage.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nlang
{

//One build's view of the package artifacts on the library search path
//(declared in ModuleBuilderImports.h). A package opens at most once, a
//search dir's package list is read at most once, and a name that no
//artifact answers for is not looked up again.
struct ArtifactPackageCache
{
	//A package met during the build: its reader, or the text Open failed
	//with (such a package stays unusable for the build).
	struct Package
	{
		std::unique_ptr<NcuPackageReader> upReader;
		std::string openError;
	};
	//Search dir -> its packages (*.npkg), file name order.
	std::map<std::string, std::vector<std::string>> packagesInDir;
	//Every package opened, by path.
	std::map<std::string, Package> packages;
	//Packages that failed to open, in the order the lookups met them.
	std::vector<std::string> unreadable;
	//Import names no usable artifact answers for (absent, or refused —
	//the lookup that met the refusal already reported it).
	std::set<std::string> absent;
	//Import name -> every file answering for it in the first search dir
	//that answered. More than one entry is a duplicate member.
	std::map<std::string, std::vector<std::string>> providers;
};

//Seam declaration in ModuleBuilderImports.h. Defined here, where the
//cache type is complete: the environment layer only passes the pointer.
std::shared_ptr<ArtifactPackageCache> MakeArtifactPackageCache()
{
	return std::make_shared<ArtifactPackageCache>();
}

namespace {

bool IsMember(const std::vector<std::string> &memberPaths,
	const std::string &name)
{
	return std::find(memberPaths.begin(), memberPaths.end(), name)
		!= memberPaths.end();
}

//The packages of one search dir, *.npkg in file name order (the
//directory is read on the first lookup only).
const std::vector<std::string> &PackagesInDir(const std::string &dir,
	ArtifactPackageCache &cache)
{
	const auto inserted = cache.packagesInDir.emplace(dir,
		std::vector<std::string>());
	std::vector<std::string> &packages = inserted.first->second;
	if (!inserted.second)
		return packages;
	std::error_code fsError;
	for (std::filesystem::directory_iterator it(dir, fsError), end;
		!fsError && it != end; it.increment(fsError))
	{
		if (it->is_regular_file(fsError)
			&& it->path().extension() == NPKG_EXTENSION)
			packages.push_back(it->path().string());
	}
	std::sort(packages.begin(), packages.end());
	return packages;
}

//Open one package at most once per build: its record keeps the reader,
//or the failure (which also joins the unreadable list).
const ArtifactPackageCache::Package &OpenPackage(const std::string &path,
	ArtifactPackageCache &cache)
{
	const auto inserted = cache.packages.emplace(path,
		ArtifactPackageCache::Package());
	ArtifactPackageCache::Package &package = inserted.first->second;
	if (!inserted.second)
		return package;
	auto upReader = std::make_unique<NcuPackageReader>();
	std::string error;
	if (upReader->Open(path, &error))
		package.upReader = std::move(upReader);
	else
	{
		package.openError = error;
		cache.unreadable.push_back(path);
	}
	return package;
}

//Make image the unit found for name. The run-time loader's identity
//rule (NcuLoader::AcceptUnit) applies at compile time too: whatever
//answers for module X must carry module path X. A mislabeled artifact
//is refused here instead of passing the build and failing only when the
//program runs.
void AdoptUnitImage(ArtifactUnit &unit, const std::string &name,
	CompiledModule image, const std::string &artifactPath)
{
	if (image.modulePath != name)
	{
		unit.error = "unit found for module '" + name + "' ('"
			+ artifactPath + "') names module '" + image.modulePath
			+ "' in its header";
		return;
	}
	unit.found = true;
	unit.image = std::move(image);
	unit.artifactPath = artifactPath;
}

//The unit file answering for name: read and adopted, or refused with
//the loader's shape for an unreadable unit file.
void ReadUnitFile(const std::string &path, const std::string &name,
	ArtifactUnit &unit)
{
	try
	{
		AdoptUnitImage(unit, name, ModuleLoader::Load(path), path);
	}
	catch (const std::exception &e)
	{
		unit.error = "'" + path + "': " + e.what();
	}
}

//One package member answering for name: extracted with its checksum
//verified, then parsed. The extract error is the reader's own text (it
//names package and member); a parse failure is reported under the same
//file-relative shape as a unit file's.
void ReadPackageMember(const NcuPackageReader &package,
	const std::string &path, const std::string &name, ArtifactUnit &unit)
{
	std::string bytes;
	std::string error;
	if (!package.ExtractMember(name, &bytes, &error))
	{
		unit.error = error;
		return;
	}
	try
	{
		AdoptUnitImage(unit, name,
			ModuleLoader::LoadFromBytes(path, bytes), path);
	}
	catch (const std::exception &e)
	{
		unit.error = "'" + path + "': " + e.what();
	}
}

//Look for name in one search dir, in the run-time loader's order: the
//unit file <dir>/<name>.ncu, then the member tables of the dir's
//packages. A unit file shadows the dir's packages. When a package
//carries the member, the dir's remaining packages still open, so that
//providers ends up listing every file of the dir answering for the
//name. True when a file answered (found or refused): the search ends
//with this dir.
bool FindArtifactInDir(const std::string &dir, const std::string &name,
	ArtifactPackageCache &cache, ArtifactUnit &unit,
	std::vector<std::string> &providers)
{
	const std::string unitFile = dir + "/" + name + NCU_EXTENSION;
	std::error_code fsError;
	if (std::filesystem::is_regular_file(unitFile, fsError))
	{
		ReadUnitFile(unitFile, name, unit);
		providers.push_back(unitFile);
		return true;
	}
	for (const std::string &path : PackagesInDir(dir, cache))
	{
		const ArtifactPackageCache::Package &package = OpenPackage(path, cache);
		if (!package.upReader
			|| !IsMember(package.upReader->MemberPaths(), name))
			continue;
		providers.push_back(path);
		//The first provider answering is the one adopted.
		if (providers.size() == 1)
			ReadPackageMember(*package.upReader, path, name, unit);
	}
	return !providers.empty();
}

} //namespace

//Seam declaration in ModuleBuilderImports.h.
ArtifactUnit FindArtifactUnit(const BuildParams &params,
	ArtifactPackageCache &cache, const std::string &name)
{
	ArtifactUnit unit;
	if (cache.absent.count(name) > 0)
		return unit;
	for (const std::string &dir : EffectiveLibraryDirs(params))
	{
		std::vector<std::string> providers;
		if (!FindArtifactInDir(dir, name, cache, unit, providers))
			continue;
		if (!unit.error.empty())
			cache.absent.insert(name);
		cache.providers.emplace(name, std::move(providers));
		return unit;
	}
	cache.absent.insert(name);
	return unit;
}

//Discovery arm for a name that no library source answers for: look its
//artifact up and queue it for the load phase. A name is queued once
//however many units import it. A refusal is reported here, by the
//lookup that met it; an absent name (nothing answers for it) is the
//CALLER's report — the mirror-record resolver emits the module-not-
//found diagnostic, the import gate owns a TU spec's name.
ModuleBuilder::ArtifactOutcome ModuleBuilder::DiscoverArtifactUnit(
	const std::string &name)
{
	if (HasPendingArtifact(name))
		return ArtifactOutcome::Queued;
	ArtifactUnit unit = FindArtifactUnit(m_upEnv->Params(),
		*m_upEnv->ArtifactPackages(), name);
	if (!unit.error.empty())
	{
		m_upEnv->Log(CLL_Error, "Failed to load module '%s': %s",
			name.c_str(), unit.error.c_str());
		return ArtifactOutcome::Refused;
	}
	if (unit.found)
	{
		PendingArtifactLoad pending;
		pending.name = unit.image.modulePath;
		pending.image = std::move(unit.image);
		pending.artifactPath = unit.artifactPath;
		m_pendingArtifacts.push_back(std::move(pending));
		return ArtifactOutcome::Queued;
	}
	return ArtifactOutcome::Absent;
}

//True when an artifact unit of that module path is already queued
//(the pending set only ever appends, so this is the queue's dedup).
bool ModuleBuilder::HasPendingArtifact(const std::string &name) const
{
	for (const PendingArtifactLoad &pending : m_pendingArtifacts)
	{
		if (pending.name == name)
			return true;
	}
	return false;
}

//Register a registry entry for every artifact unit the discovery phase
//queued. Runs before the import gates are built: a gate then finds an
//artifact unit the way it finds a compiled-in one, as a registered
//name. The entry's index goes back to the queued unit, whose stub
//table the load phase fills.
void ModuleBuilder::MaterializeArtifacts()
{
	ModuleRegistry &reg = m_upEnv->Registry();
	for (PendingArtifactLoad &pending : m_pendingArtifacts)
		pending.extIdx = reg.AddExternalModule(pending.name);
}

//Load phase of the queued artifact units: build the stub nodes from the
//images read at discovery (the disk is not read again) and fill each
//unit's registry entry. False after logging a failure.
bool ModuleBuilder::LoadExternalModules()
{
	for (PendingArtifactLoad &pending : m_pendingArtifacts)
	{
		CompiledModuleNodeBuilder builder(TheAST(), pending.name,
			&m_upEnv->Registry());
		try
		{
			builder.BuildFromCompiledModule(pending.image);
		}
		catch (const std::exception &e)
		{
			m_upEnv->Log(CLL_Error, "%s", e.what());
			return false;
		}
		RegisterExternalStubs(builder, pending.extIdx);
		if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
			m_upEnv->Log(CLL_Info, "Loaded module '%s' from %s",
				pending.name.c_str(), pending.artifactPath.c_str());
		m_loadedImports.push_back(std::move(pending.image));
	}
	return true;
}

void ModuleBuilder::RegisterExternalStubs(
	CompiledModuleNodeBuilder &builder, uint32_t extIdx)
{
	//Import visibility (C1): the unit's registry entry (created by
	//MaterializeArtifacts) owns every free-function stub it contributed,
	//tagged at the same point the stubs join the root — after
	//MergeTransUnits tagged the TU members, whose merged root the stub
	//collision checks need. Class methods are not free functions (they
	//stay inside their class stub), so this table is the whole
	//qualified-call surface for v1.
	ModuleRegistry &reg = m_upEnv->Registry();
	NLANG_ASSERT(extIdx < reg.ModuleCount() && reg.IsExternal(extIdx));
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

} //namespace nlang
