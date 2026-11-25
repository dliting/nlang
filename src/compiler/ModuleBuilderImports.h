/*---
ModuleBuilderImports.h - internal seam of the ModuleBuilder import
translation units: declarations shared between ModuleBuilderImports.cpp
and the split external-artifact TU (ModuleBuilderExternalImports.cpp).
---*/
#pragma once
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/NcuPackage.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nlang
{

struct BuildParams;
class BuildEnvironment;
//One build's view of the package artifacts on the library search path
//(the cache TU - ModuleBuilderArtifactCache.cpp - owns its lazy
//enumeration and knowability probe; the external-artifact TU looks
//units up through it). A package opens at most once, a search dir's
//package list is read at most once, and a name that no artifact
//answers for is not looked up again.
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

//True when the member paths carry the module path verbatim. Definition
//in ModuleBuilderArtifactCache.cpp.
bool CarriesMember(const std::vector<std::string> &memberPaths,
	const std::string &name);

//The packages of one search dir, *.npkg in file name order. Lazy: the
//directory is read on the first call only. Definition in
//ModuleBuilderArtifactCache.cpp.
const std::vector<std::string> &PackagesInDir(const std::string &dir,
	ArtifactPackageCache &cache);

//Open one package at most once per build, then classify its role:
//returns the reader only for a library package (no entry record).
//A program package's member table is its executable closure, not a
//compile-time import surface; dependencies carried there must never
//compete with library providers. An open failure joins unreadable.
//Definition in ModuleBuilderArtifactCache.cpp.
const NcuPackageReader *OpenImportablePackage(const std::string &path,
	ArtifactPackageCache &cache);

//Effective library search directories: explicit/configured import dirs
//first, then the stdlib dir, so the standard library is discoverable even
//when a direct BuildParams caller did not list it (the tool layer normally
//prepends it through BuildLibrarySearchPath). Shared by the library-source
//discovery and the artifact lookup; definitions in BuildEnvironment.cpp
//(see there for why not in the import TUs).
std::vector<std::string> EffectiveLibraryDirs(const BuildParams &params);

//Every (file, root) match for a dotted package name across the effective
//library dirs: <dir>/<a/b/c>.n, every root that offers it (the
//duplicate-package diagnosis scans the full set instead of stopping at
//the first hit). Free over BuildParams so the resolver's knowability
//probe (BuildEnvironment::HasLibrarySource) shares the one discovery
//rule the import walk uses. Definition in BuildEnvironment.cpp.
std::vector<std::pair<std::string, std::string>>
FindLibrarySourceMatches(const BuildParams &params,
	const std::string &dottedName);

//Outcome of one import name's artifact lookup.
//  found        an artifact answered for the name and its image was read
//  image        the unit's compiled module (valid when found)
//  artifactPath the unit file, or the package carrying the member
//  error        an artifact answered but was refused (unreadable bytes,
//               a header naming another module): the lookup's caller
//               reports it. Not found is found == false with no error.
struct ArtifactUnit
{
	bool found = false;
	CompiledModule image;
	std::string artifactPath;
	std::string error;
};

//Install a fresh artifact cache and the knowability probe that reads
//it into the environment, as one step: the pair is meaningless apart.
//One cache belongs to one build. Definition in
//ModuleBuilderArtifactCache.cpp.
void InstallArtifactCache(BuildEnvironment &env);

//The cache's unreadable-package list for the not-found report's Note
//lines; a build that ran no import pipeline installed no cache, so
//nothing is unreadable. Definition in ModuleBuilderArtifactCache.cpp.
const std::vector<std::string> &UnreadablePackagesOf(
	const BuildEnvironment &env);

//Locate the artifact of one import name across the effective library
//dirs, in order: the unit file <dir>/<name>.ncu first, then the member
//tables of the dir's packages (*.npkg, file name order). The first dir
//that answers ends the search. The cache keeps the packages opened and
//the names nothing answers for between lookups. Definition in
//ModuleBuilderExternalImports.cpp.
ArtifactUnit FindArtifactUnit(const BuildParams &params,
	ArtifactPackageCache &cache, const std::string &name);

//Every package member across the effective library dirs whose module
//path is the dotted name itself or lies under it - a wildcard union's
//artifact arm (the source arm is the disk subtree walk). All dirs
//contribute: the union only names candidates, and the per-name
//first-dir-wins rule stays the resolver's (HandleLibraryImport).
//Sorted and deduped so the queue order is deterministic. Definition in
//ModuleBuilderExternalImports.cpp.
std::vector<std::string> CollectPackageMemberUnion(
	const BuildParams &params, ArtifactPackageCache &cache,
	const std::string &dottedName);

} //namespace nlang
