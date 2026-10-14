/*---
    ScriptRunner.h -- in-process compile+execute pipeline for the nlang
    tool (v0.9.1 script mode). Single-header implementation, mirroring
    the ncc SearchPathArgs.h tool-header pattern. The pipeline is the
    "ncc <source.n>" compile-and-run seam (ncc/main.cpp:409-470):
    BuildArtifacts writes the entry unit .ncu into a per-process temp
    directory, then NcuLoader::LoadClosure -> NcuLinker::Link ->
    VmExecutor::Execute. Library translation units (stdlib and -I source
    libraries) are excluded from codegen by design -- their code ships
    in their own packages and joins the closure at load time -- so the
    loader (not the in-memory unit images) is what completes the
    closure; every in-process consumer (ncc run mode, ndb, embed) uses
    the same artifact-entry seam.
---*/
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>   //GetCurrentProcessId/GetModuleFileNameA
#else
#include <unistd.h>    //getpid
#endif
#include "nlang/common/LibrarySearchPath.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/langservice/SymbolIndex.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "NcuLinker.h"
#include "NcuLoader.h"
#include "TestNatives.h"
#include "VmExecutor.h"

namespace nlangtool {

//The pipeline below is written against the nlang:: API surface; the
//using-directive stays inside nlangtool so includers see nothing new.
using namespace nlang;

//Outcome of one script run. ok=false means the script never executed
//(compile/link/internal error); the exit code is then the tool's own 1.
struct RunOutcome {
    int exitCode = 1;
    bool ok = false;
};

//Quiet-on-success logger: an ad-hoc runner reports diagnostics
//(warnings/errors/fatals), not the compiler's progress lines — the
//"Output module file" info would name a temp artifact that is deleted
//before the reader could look at it. Compile warnings and errors ride
//different levels and pass through unchanged.
class DiagnosticOnlyLogger : public StreamCompileLogger {
public:
    explicit DiagnosticOnlyLogger(std::ostream& stream)
        : StreamCompileLogger(stream) {}
protected:
    void WriteLog(CompileLogLevel level, const ISourceLocation* location,
                  const char* message) override {
        if (level != CLL_Info)
            StreamCompileLogger::WriteLog(level, location, message);
    }
};

//This process's id, for unique temp names (the classes below prefix
//their paths with it so concurrent runs never collide).
inline unsigned ProcessId() {
#ifdef _WIN32
    return static_cast<unsigned>(GetCurrentProcessId());
#else
    return static_cast<unsigned>(::getpid());
#endif
}

//Shared temp root via the non-throwing overload: both RAII classes
//below report their own I/O failures with a diagnostic, so the root
//lookup must not throw past them (no usable TEMP on Windows throws
//filesystem_error, and nothing in main catches it). Returns an empty
//path after printing; callers treat empty as "cannot proceed".
inline std::filesystem::path TempRoot() {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec);
    if (ec) {
        std::fprintf(stderr, "nlang: cannot locate temp directory: %s\n",
            ec.message().c_str());
        return {};
    }
    return root;
}

//RAII carrier for `nlang -c "<statements>"`: the statements become a
//temp .n file so line numbers survive diagnostics (raw UTF-8 content,
//no textual wrapping). Removed on every exit path, including exceptions.
//The file stem doubles as the module name, so it stays an identifier:
//underscore separators only, no dashes (a dash is invalid in module
//names — "Create module failed, invalid module name").
class TempScriptFile {
public:
    explicit TempScriptFile(const std::string& utf8Content) {
        const std::filesystem::path root = TempRoot();
        if (root.empty())
            return;   //diagnostic already printed; Path() stays empty
        static std::atomic<unsigned> counter{0};
        m_path = root / ("nlang_c_" + std::to_string(ProcessId()) + "_"
            + std::to_string(counter++) + ".n");
        std::ofstream out(m_path, std::ios::binary);
        out << utf8Content;
        //Disk-full/permission failure surfaces here; without this the
        //pipeline would only report a confusing "cannot open" compile
        //error for a file the tool itself failed to create.
        if (!out)
            std::fprintf(stderr,
                "nlang: cannot create temp script file: %s\n",
                m_path.string().c_str());
    }
    ~TempScriptFile() {
        std::error_code ignored;
        std::filesystem::remove(m_path, ignored);
    }
    TempScriptFile(const TempScriptFile&) = delete;
    TempScriptFile& operator=(const TempScriptFile&) = delete;
    const std::filesystem::path& Path() const { return m_path; }
private:
    std::filesystem::path m_path;
};

//RAII scratch directory under the temp root, unique per run (pid +
//counter): BuildArtifacts writes the entry unit .ncu here (its name is
//<module stem>.ncu, so a shared directory would race two same-stem
//scripts run concurrently). The whole directory is removed on every
//exit path; the module name stays the script's own stem, so backtraces
//read <script>.main.
class TempRunDir {
public:
    TempRunDir() {
        const std::filesystem::path root = TempRoot();
        if (root.empty())
            return;   //diagnostic already printed; Path() stays empty
        static std::atomic<unsigned> counter{0};
        m_path = root / ("nlang_r_" + std::to_string(ProcessId()) + "_"
            + std::to_string(counter++));
        std::error_code ec;
        std::filesystem::create_directories(m_path, ec);
        if (ec)
            std::fprintf(stderr,
                "nlang: cannot create temp run directory: %s: %s\n",
                m_path.string().c_str(), ec.message().c_str());
    }
    ~TempRunDir() {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }
    TempRunDir(const TempRunDir&) = delete;
    TempRunDir& operator=(const TempRunDir&) = delete;
    const std::filesystem::path& Path() const { return m_path; }
private:
    std::filesystem::path m_path;
};

//This executable's own path (ncc's form: the manifest makes the ANSI
//form UTF-8 on Windows; /proc on POSIX).
inline std::filesystem::path ExecutablePath() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf);
#else
    return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

//One search list serves compile-time import discovery, closure loading
//and native module resolution (the ONE-list discipline), composed with
//the shared layered builder so nlang matches ncc exactly (spec D8 /
//"搜索路径 nccsearch 同式"): -I > the script's own directory >
//NLANG_PATH > {stdlib, exe dir, cwd}, normalized and deduped by the
//builder itself. The script directory is skipped when it is only the
//tool's temp root (a -c payload): that directory is an artifact of the
//tool, not user intent — a stray stdlib-named .n dropped in the shared
//temp root must not shadow the real library for -c runs.
inline std::vector<std::string> ResolveSearchDirs(
        const std::vector<std::filesystem::path>& importDirs,
        const std::filesystem::path& script,
        const std::string& stdlibDir) {
    SearchPathInput search;
    search.explicitDirs.reserve(importDirs.size());
    for (const auto& dir : importDirs)
        search.explicitDirs.push_back(dir.string());
    const std::filesystem::path scriptDir =
        std::filesystem::absolute(script).parent_path();
    //Silent lookup (no diagnostic here): this predicate only skips the
    //directory, and the temp-writing classes own the failure reporting
    //when the root itself is unusable. equivalent(), not path ==: the
    //same directory arrives in different spellings (temp_directory_path
    //ends with a separator, parent_path never does), and path equality
    //is lexical.
    std::error_code rootEc;
    const std::filesystem::path tempRoot =
        std::filesystem::temp_directory_path(rootEc);
    std::error_code sameRootEc;
    const bool isToolTempRoot = !scriptDir.empty() && !rootEc
        && std::filesystem::equivalent(scriptDir, tempRoot, sameRootEc);
    if (!scriptDir.empty() && !isToolTempRoot)
        search.baseDirs.push_back(scriptDir.lexically_normal().string());
    if (const char* env = std::getenv("NLANG_PATH"))
        search.pathEnv = env;
    search.systemDirs = {stdlibDir,
        ExecutablePath().parent_path().string(), "."};
    const std::vector<SearchDirEntry> traced =
        BuildLibrarySearchPathLayered(search);
    std::vector<std::string> out;
    out.reserve(traced.size());
    for (const auto& entry : traced)
        out.push_back(entry.dir);
    return out;
}

//Run tail, the ncc compile-and-run sequence after the artifact exists:
//the loader completes the closure from the search dirs (stdlib.npkg
//members, third-party .ncu packages, -I source library packages), the
//linker resolves the import slots, the executor runs the entry point.
//An empty script links without an entry point and maps to exit 0
//(spec D3). One boundary covers load/link/execute and prints the
//"Runtime error: ..." shape every tool uses (ncc:477-481, nvm:193-194),
//so anything grepping that surface sees closure failures too; the
//backtrace stays empty unless a frame actually ran.
inline RunOutcome ExecuteArtifact(const std::filesystem::path& artifact,
                                  const std::vector<std::string>& searchDirs) {
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = searchDirs;
    VmExecutor executor;
    //Phase 9f host natives: the fifth execution surface keeps parity
    //with nvm/ncc/ndb/ndisasm (four-tool precedent).
    RegisterTestNatives(executor);
    for (const auto& dir : searchDirs)
        executor.AddNativeSearchDir(dir);
    try {
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(artifact.string(), loaderOpts);
        const CompiledModule linked =
            NcuLinker::Link(loaded.units, loaded.entryKey);
        //Empty script (D3): linked but no entry point. Executing anyway
        //would throw inside the executor; exit 0 like a no-op program.
        if (linked.entryPoint < 0)
            return {0, true};
        return {executor.Execute(linked), true};
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Runtime error: %s\n", e.what());
        const std::string& bt = executor.Backtrace();
        if (!bt.empty())
            std::fprintf(stderr, "Backtrace:\n%s", bt.c_str());
        return {};
    }
}

//BuildParams for one script compile: a single source file, the artifact
//written into runDir, and the import dirs REPLACED with the resolved
//search list (BuildParams' constructor seeds a literal "." and the list
//already carries the normalized working dir — the ncc
//m_ImportDirs.clear() trap).
inline BuildParams ScriptBuildParams(const std::filesystem::path& script,
        const std::string& stdlibDir,
        const std::vector<std::string>& searchDirs,
        const std::filesystem::path& runDir) {
    BuildParams params;
    params.m_SourceFiles.push_back(
        std::filesystem::absolute(script).string());
    params.m_sProjectDir =
        std::filesystem::absolute(script).parent_path().string();
    params.m_sOutputModule = script.stem().string();
    params.m_sOutputDir = runDir.string();
    params.m_sTempDir = params.m_sOutputDir;
    params.m_sStdLibDir = stdlibDir;
    params.m_ImportDirs.assign(searchDirs.begin(), searchDirs.end());
    return params;
}

//Compile and run one script in process (the "ncc <source.n>" seam):
//BuildArtifacts writes the entry unit .ncu to a per-run temp dir, then
//ExecuteArtifact runs it. Compile diagnostics stream to stderr through
//the logger.
inline RunOutcome RunScript(const std::filesystem::path& script,
                            const std::vector<std::filesystem::path>& importDirs) {
    //Static tables before any compiler/VM work: Runtime::StaticInit owns
    //the IdString intern tables (building any module dereferences them),
    //then the compiler-side cast table — same order as ncc. Both are
    //idempotent, and --version exits long before this point.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    const std::string stdlibDir = langservice::FindStdLibDir(
        ExecutablePath().parent_path().string());
    const std::vector<std::string> searchDirs =
        ResolveSearchDirs(importDirs, script, stdlibDir);

    TempRunDir runDir;
    if (runDir.Path().empty())
        return {};   //diagnostic already printed (no usable temp root)
    BuildParams params =
        ScriptBuildParams(script, stdlibDir, searchDirs, runDir.Path());

    DiagnosticOnlyLogger logger(std::cerr);
    bool built = false;
    try {
        ModuleBuilder builder(params, logger);
        //Single-file build: writes <runDir>/<stem>.ncu (the entry unit
        //image; its import slots ride along).
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Compiler internal error: %s\n", e.what());
        return {};
    }
    if (!built) {
        //Diagnostics already went to stderr through the logger; give
        //the failed run the same terminal marker ncc prints.
        std::fprintf(stderr, "Compilation failed.\n");
        return {};
    }
    return ExecuteArtifact(
        runDir.Path() / (script.stem().string() + ".ncu"), searchDirs);
}

} //namespace nlangtool
