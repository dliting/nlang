/*---
ModuleBuilderImports.h - internal seam of the ModuleBuilder import
translation units: declarations shared between ModuleBuilderImports.cpp
and the split external-artifact TU (ModuleBuilderExternalImports.cpp).
---*/
#pragma once
#include <string>
#include <vector>

namespace nlang
{

struct BuildParams;

//Effective library search directories: explicit/configured import dirs
//first, then the stdlib dir, so the standard library is discoverable even
//when a direct BuildParams caller did not list it (the tool layer normally
//prepends it through BuildLibrarySearchPath). Definition in
//ModuleBuilderImports.cpp; shared by the library-source discovery and the
//external .ncu/.npkg lookup.
std::vector<std::string> EffectiveLibraryDirs(const BuildParams &params);

} //namespace nlang
