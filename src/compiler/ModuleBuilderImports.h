/*---
ModuleBuilderImports.h - internal seam of the ModuleBuilder import
translation units: declarations shared between ModuleBuilderImports.cpp
and the split external-artifact TU (ModuleBuilderExternalImports.cpp).
---*/
#pragma once
#include <nlang/vm/CompiledModule.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nlang
{

struct BuildParams;
//One build's view of the package artifacts on the library search path.
//Opaque here: the type is private to ModuleBuilderExternalImports.cpp.
struct ArtifactPackageCache;

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

//A fresh artifact cache; one belongs to one build.
//Definition in ModuleBuilderExternalImports.cpp.
std::shared_ptr<ArtifactPackageCache> MakeArtifactPackageCache();

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
