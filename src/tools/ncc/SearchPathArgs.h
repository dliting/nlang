/*--- SearchPathArgs.h - compose ncc's import search path ---*/
#ifndef NLANG_NCC_SEARCH_PATH_ARGS_H
#define NLANG_NCC_SEARCH_PATH_ARGS_H

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "ProjectFile.h"
#include "nlang/common/LibrarySearchPath.h"

namespace nccsearch {

//Assemble the unified, ordered library search path for a compile/build
//invocation: CLI -I > project <ImportPaths> > local source/project dir >
//NLANG_PATH > stdlib/exe/cwd. The same dirs drive .n source discovery and
//native DLL loading. Pure (the resolver does no existence checks). The
//layered result keeps per-directory attribution for --verbose.
inline std::vector<nlang::SearchDirEntry> ResolveCompileImportDirs(
    const std::vector<std::string>& cliDirs,
    const nlang::ProjectFile& project,
    const std::string& sourceFile,
    const std::string& stdlibDir,
    const std::filesystem::path& exePath) {
    nlang::SearchPathInput search;
    search.explicitDirs = cliDirs;
    if (!project.projectDir.empty()) {
        search.configuredDirs = project.importPaths;
        search.baseDirs.push_back(project.projectDir);
    } else {
        const std::filesystem::path parent =
            std::filesystem::path(sourceFile).parent_path();
        if (!parent.empty())
            search.baseDirs.push_back(parent.string());
    }
    if (const char* env = std::getenv("NLANG_PATH"))
        search.pathEnv = env;
    search.systemDirs = {
        stdlibDir, exePath.parent_path().string(), "."
    };
    return nlang::BuildLibrarySearchPathLayered(search);
}

} // namespace nccsearch

#endif
