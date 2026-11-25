/*---
    NcuPackageCache.h — native-member extraction cache. Where a package
    carries native host libraries, the run-time loader cannot open them
    inside the archive: the operating system needs a real file. This is
    the extraction seam (package_artifact_design.md D5.2) — extraction
    happens when the closure walk opens the package, and the laziness
    boundary is module LOADING, which stays in NativeLibraryLoader.
    Internal header; the public API is nlang/vm/NcuPackage.h.
---*/
#pragma once
#include <string>
#include <vector>

namespace nlang
{

class NcuPackageReader;

//Extracts every native member of pkg into the package's cache
//directory under the cache root (created on demand), keyed by a stable
//hash of the absolute package path — two packages never collide, and
//re-running the same package lands in the same place. A cached file
//whose bytes match the member is left alone (the landing check); a
//mismatching file is overwritten through a process-unique temporary
//name and an atomic rename, so a concurrent process at worst sees the
//old complete file. Appends the package's cache directory to
//cacheDirs when the package carries at least one native member.
//Failure to resolve or create the cache root reports through *error
//and extracts nothing; the caller decides throw-versus-collect.
void ExtractNativeMembersToCache(const NcuPackageReader& pkg,
    const std::string& packagePath, std::vector<std::string>& cacheDirs,
    std::string* error);

} //namespace nlang
