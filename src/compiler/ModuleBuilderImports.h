/*---
ModuleBuilderImports.h - internal seam of the ModuleBuilder import
translation units: declarations shared between ModuleBuilderImports.cpp
and the split external-artifact TU (ModuleBuilderExternalImports.cpp).
---*/
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace nlang
{

struct BuildParams;

//Effective library search directories: explicit/configured import dirs
//first, then the stdlib dir, so the standard library is discoverable even
//when a direct BuildParams caller did not list it (the tool layer normally
//prepends it through BuildLibrarySearchPath). Shared by the library-source
//discovery and the external .ncu/.npkg lookup; definitions in
//BuildEnvironment.cpp (see there for why not in the import TUs).
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

} //namespace nlang
