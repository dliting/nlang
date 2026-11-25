/*-----------------------------------------------------------------------------
	ModuleBuilderArtifactCache.cpp
	The artifact cache the import pipeline installs into the build
	environment: one build's lazy view of the package artifacts on the
	library search path - a search dir's package list read at most once,
	a package opened at most once - and the knowability probe the
	diagnostic fold's artifact arm consults
	(BuildEnvironment::ArtifactKnowability). Split from
	ModuleBuilderExternalImports.cpp (2026-11-25).
-----------------------------------------------------------------------------*/

#include "ModuleBuilderImports.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nlang
{

//InstallArtifactCache's cache half (the probe half is the file-local
//ArtifactAnswersFor below). The environment layer only passes the
//pointer: the reader lives here, with the cache.
static std::shared_ptr<ArtifactPackageCache> MakeArtifactPackageCache()
{
	return std::make_shared<ArtifactPackageCache>();
}

//Seam declaration in ModuleBuilderImports.h.
bool CarriesMember(const std::vector<std::string> &memberPaths,
	const std::string &name)
{
	return std::find(memberPaths.begin(), memberPaths.end(), name)
		!= memberPaths.end();
}

//Seam declaration in ModuleBuilderImports.h.
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

//Cache-internal open primitive. OpenImportablePackage below is the
//only seam exposed to artifact discovery, so program packages cannot
//accidentally join one compiler scan while another correctly skips them.
static const ArtifactPackageCache::Package &OpenPackage(
	const std::string &path, ArtifactPackageCache &cache)
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

//Seam declaration in ModuleBuilderImports.h. Program packages carry
//an entry record and a complete executable closure; only entry-less
//library packages expose their members to compile-time imports.
const NcuPackageReader *OpenImportablePackage(const std::string &path,
	ArtifactPackageCache &cache)
{
	const ArtifactPackageCache::Package &package = OpenPackage(path, cache);
	if (!package.upReader || package.upReader->EntryRecord())
		return nullptr;
	return package.upReader.get();
}

//The artifact arm's knowability probe (BuildEnvironment's
//ArtifactKnowability calls it through the installed pointer):
//knowability only, no read and no adoption. The scan is lazy - dir
//listings and opened packages join the cache the discovery lookups
//share, so a fold query after the fixed point reuses it.
static bool ArtifactAnswersFor(const BuildEnvironment &env,
	ArtifactPackageCache &cache, const std::string &name)
{
	const std::string unitFile = name + NCU_EXTENSION;
	for (const std::string &dir : EffectiveLibraryDirs(env.Params()))
	{
		std::error_code fsError;
		if (std::filesystem::is_regular_file(
				std::filesystem::path(dir) / unitFile, fsError))
			return true;
		for (const std::string &path : PackagesInDir(dir, cache))
		{
			const NcuPackageReader *pPackage =
				OpenImportablePackage(path, cache);
			if (pPackage
				&& CarriesMember(pPackage->MemberPaths(), name))
				return true;
		}
	}
	return false;
}

//Seam declaration in ModuleBuilderImports.h. The cache and its
//knowability probe enter the environment together: one without the
//other answers no query (the environment's guard checks the pair).
void InstallArtifactCache(BuildEnvironment &env)
{
	env.ResetArtifactPackages(MakeArtifactPackageCache());
	env.SetArtifactKnowabilityProbe(&ArtifactAnswersFor);
}

//Seam declaration in ModuleBuilderImports.h.
const std::vector<std::string> &UnreadablePackagesOf(
	const BuildEnvironment &env)
{
	static const std::vector<std::string> kEmpty;
	const std::shared_ptr<ArtifactPackageCache> &spCache =
		env.ArtifactPackages();
	return spCache ? spCache->unreadable : kEmpty;
}

} //namespace nlang
