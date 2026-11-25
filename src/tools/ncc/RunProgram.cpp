/*---
    RunProgram.cpp — implementation of the `ncc run` execution path
    (RunProgram.h contract).
---*/
#include "RunProgram.h"
#include "NativeLibraryLoader.h"
#include "NcuLinker.h"
#include "NcuLoader.h"
#include "SearchPathArgs.h"
#include "TestNatives.h"
#include "VmExecutor.h"
#include <nlang/langservice/SymbolIndex.h>
#include <nlang/vm/CompiledModule.h>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace nlang
{
namespace
{

namespace fs = std::filesystem;

//The layered search for a run: -I dirs, the artifact's own directory,
//NLANG_PATH, then the system roots (exe directory, stdlib, cwd);
//verbose prints the per-directory attribution. Order matters: a loose
//library on an earlier layer outranks its packed copy.
std::vector<std::string> AssembleRunSearchDirs(const std::string& artifact,
    const std::vector<std::string>& explicitDirs, bool verbose)
{
    SearchPathInput search;
    search.explicitDirs = explicitDirs;
    search.baseDirs.push_back(fs::path(artifact).parent_path().string());
    if (const char* env = std::getenv("NLANG_PATH"))
        search.pathEnv = env;
    search.systemDirs = {NativeLibraryLoader::ExecutableDir(), "."};
    std::vector<SearchDirEntry> tracedDirs =
        BuildLibrarySearchPathLayered(search);
    const std::string stdlibDir = langservice::FindStdLibDir(
        NativeLibraryLoader::ExecutableDir());
    if (!stdlibDir.empty())
        tracedDirs.push_back({stdlibDir, SearchLayer::System});
    if (verbose)
        std::cout << FormatSearchDirs(tracedDirs) << "\n";
    std::vector<std::string> allDirs;
    allDirs.reserve(tracedDirs.size());
    for (const auto& e : tracedDirs)
        allDirs.push_back(e.dir);
    return allDirs;
}

} //namespace

int RunProgramArtifact(const std::string& artifact,
    const std::vector<std::string>& explicitDirs, bool verbose)
{
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    try {
        const std::vector<std::string> allDirs =
            AssembleRunSearchDirs(artifact, explicitDirs, verbose);
        for (const auto& dir : allDirs)
            executor.AddNativeSearchDir(dir);
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = allDirs;
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(artifact, loaderOpts);
        //Native members ride packages as cached copies (D5.2): the
        //receipt names where they landed, so the executor finds them
        //after the load (before it, the set is always empty).
        for (const auto& cacheDir : loaded.nativeCacheDirs)
            executor.AddNativeSearchDir(cacheDir);
        CompiledModule mod = NcuLinker::Link(loaded.units, loaded.entryKey);
        return executor.Execute(mod);
    } catch (const std::exception& e) {
        std::cerr << "Runtime error: " << e.what() << "\n";
        const auto& bt = executor.Backtrace();
        if (!bt.empty())
            std::cerr << "Backtrace:\n" << bt;
        return 1;
    }
}

} //namespace nlang
