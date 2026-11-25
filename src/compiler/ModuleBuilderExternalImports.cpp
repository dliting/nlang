/*-----------------------------------------------------------------------------
	ModuleBuilderExternalImports.cpp
	The external-artifact arm of the import pipeline: the discovery-phase
	lookup of an import name's artifact (a unit file or a package member on
	the library search path, through the artifact cache the pipeline
	installs - ModuleBuilderArtifactCache.cpp), and the load-phase
	registration of the found units — their stub nodes, registry entries
	and stub tables — and the
	mirror-record arm: resolving the import-record names a queued image
	carries (the discovery fixed point's second driver). Split from
	ModuleBuilderImports.cpp (2026-11-19 maintainability refactor).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "BuiltinPrelude.h"
#include "ModuleBuilderImportCycles.h"
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

namespace {

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
		const NcuPackageReader *pPackage =
			OpenImportablePackage(path, cache);
		if (!pPackage || !CarriesUnitMember(*pPackage, name))
			continue;
		providers.push_back(path);
		//The first provider answering is the one adopted.
		if (providers.size() == 1)
			ReadPackageMember(*pPackage, path, name, unit);
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

//Seam declaration in ModuleBuilderImports.h. One wildcard union's
//artifact arm: every package member across the effective library dirs
//whose path is the name itself or lies under it. All dirs contribute
//(the per-name first-dir-wins rule is the resolver's, not the
//collector's - the union only names candidates). Sorted and deduped
//so the fixed point's queue order is deterministic.
std::vector<std::string> CollectPackageMemberUnion(
	const BuildParams &params, ArtifactPackageCache &cache,
	const std::string &dottedName)
{
	const std::string prefix = dottedName + '.';
	std::set<std::string> members;
	for (const std::string &dir : EffectiveLibraryDirs(params))
	{
		for (const std::string &path : PackagesInDir(dir, cache))
		{
			const NcuPackageReader *pPackage =
				OpenImportablePackage(path, cache);
			if (!pPackage)
				continue;
			//Unit members only: a native library's key is a file name,
			//not a module path, and never joins a wildcard union.
			const std::vector<std::string> &paths = pPackage->MemberPaths();
			const std::vector<NcuMemberKind> &kinds = pPackage->MemberKinds();
			for (size_t i = 0; i < paths.size(); ++i)
			{
				if (kinds[i] != NcuMemberKind::Unit)
					continue;
				if (paths[i] == dottedName
					|| paths[i].compare(0, prefix.size(), prefix) == 0)
					members.insert(paths[i]);
			}
		}
	}
	return { members.begin(), members.end() };
}

//The wildcard union's artifact arm (ModuleBuilder.h). True when a TU
//was added; a pass that only queues members still iterates - the
//pending set's growth is the fixed point's other signal.
bool ModuleBuilder::DiscoverMemberTableSubtree(
	const std::string &dottedName)
{
	bool changed = false;
	for (const std::string &member : CollectPackageMemberUnion(
			m_upEnv->Params(), *m_upEnv->ArtifactPackages(), dottedName))
	{
		if (HandleLibraryImport(member))
			changed = true;
	}
	return changed;
}

//One import-record name read from a loaded mirror image, resolved in
//the import order (D2). The prelude packages are inlined by the
//implicit import before discovery, and a project unit owns its own
//name (no artifact may answer for it); otherwise the library-source
//arm runs first — a TU was added, or a source answering for the name
//is already inlined — then the artifact arm's outcome separates a
//queued unit, a refused one (its diagnosis is already logged) and an
//absent one. False only when nothing answers for the name.
bool ModuleBuilder::ResolveMirrorImportName(const std::string &name)
{
	for (const PreludePackage &pkg : kPreludePackages)
	{
		if (name == pkg.name)
			return true;
	}
	if (IsProjectModuleName(name))
		return true;
	if (HandleLibraryImport(name, /*discoverArtifact=*/false))
		return true;
	//A false return also covers the deduped re-import of an
	//already-inlined source (and the duplicate-package diagnosis —
	//matches non-empty, the error already logged): both are homes.
	if (!FindLibrarySourceMatches(m_upEnv->Params(), name).empty())
		return true;
	return DiscoverArtifactUnit(name) != ArtifactOutcome::Absent;
}

//The fixed point's closing report: a record name nothing answered for
//has no gate to report it (no TU spec names it), so discovery reports
//it itself — except a name a TU spec also carries: the import gate's
//report owns that one (one diagnostic per name, not two).
void ModuleBuilder::ReportHomelessMirrorNames(
	const std::set<std::string> &specNames,
	const std::vector<std::string> &homelessNames) const
{
	//Same note as the gate's not-found report: the unreadable packages
	//of the discovery cache join the text, one line each.
	const std::vector<std::string> &unreadable =
		UnreadablePackagesOf(*m_upEnv);
	for (const std::string &name : homelessNames)
	{
		if (specNames.count(name) == 0)
			m_upEnv->Log(CLL_Error, "%s",
				ModuleNotFoundText(name, unreadable).c_str());
	}
}

//One fixed-point pass's mirror half: every artifact image queued since
//scannedArtifacts was set has its import-record names resolved, each
//name once, in record order (deterministic: the five tables serialize
//in a fixed order). A name that resolves to nothing joins
//homelessNames for the closing report. True when the pass grew the TU
//set or the pending set — the fixed point iterates on growth.
bool ModuleBuilder::ScanMirrorImportRecords(size_t &scannedArtifacts,
	std::set<std::string> &mirrorNames,
	std::vector<std::string> &homelessNames)
{
	const size_t unitsBefore = m_upTransUnits->size();
	const size_t totalArtifacts = m_pendingArtifacts.size();
	for (size_t i = scannedArtifacts; i < totalArtifacts; ++i)
	{
		//Resolving a record name below can queue another artifact unit,
		//and the pending vector reallocates when it grows: an element
		//reference does not survive that, so the self-reference check
		//reads the name through a copy and the record names live in a
		//local vector, both taken before any resolution runs.
		const std::string selfModulePath = m_pendingArtifacts[i].name;
		const std::vector<std::string> recordNames =
			ImageImportTargets(m_pendingArtifacts[i].image);
		for (const std::string &name : recordNames)
		{
			//A unit's record of its own path is a self-reference, not a
			//dependency (the cycle check drops the same shape).
			if (name == selfModulePath)
				continue;
			if (!mirrorNames.insert(name).second)
				continue;
			if (!ResolveMirrorImportName(name))
				homelessNames.push_back(name);
		}
	}
	scannedArtifacts = totalArtifacts;
	return m_pendingArtifacts.size() != totalArtifacts
		|| m_upTransUnits->size() != unitsBefore;
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
	ArtifactPackageCache &cache = *m_upEnv->ArtifactPackages();
	ArtifactUnit unit = FindArtifactUnit(m_upEnv->Params(), cache, name);
	//Two packages in one search dir offering the same member is the
	//artifact twin of the registry's duplicate-package rule: a silent
	//first-wins would bind the import to whichever package the sorted
	//scan hit first, so both package files are named and the unit is
	//refused. The absent mark keeps a later lookup (a mirror record
	//naming the refused name) from re-diagnosing — the failed-load
	//family's dedup shape.
	const auto recorded = cache.providers.find(name);
	if (unit.found && recorded != cache.providers.end()
		&& recorded->second.size() > 1)
	{
		m_upEnv->Log(CLL_Error, "Duplicate package '%s': '%s' and '%s'.",
			name.c_str(), recorded->second[0].c_str(),
			recorded->second[1].c_str());
		cache.absent.insert(name);
		return ArtifactOutcome::Refused;
	}
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
