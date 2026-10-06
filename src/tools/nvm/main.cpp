#include "NcuLoader.h"
#include "NcuLinker.h"
#include "nlang/langservice/SymbolIndex.h"
#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#include "nlang/common/LibrarySearchPath.h"
#include <nlang/runtime/Runtime.h>
#ifdef _WIN32
#include <windows.h>  //SetErrorMode/ExitProcess (was transitive via CrashReporter.h)
#endif
#include <nlang_version.h>  // generated from the repo VERSION file
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace nlang;

#ifdef _WIN32
namespace {
//Per-stream console UTF-8 setup. GetConsoleMode is the is-a-console
//test, not GetFileType: GetFileType misclassifies the NUL device as a
//character device, so `nvm x > NUL` would otherwise rewrite the code
//page of the user's interactive console from a redirected run. Each
//half is judged independently, so a half-redirected invocation only
//switches the half that is a real console. Originals are restored at
//exit (std::atexit); a crash skips that, which is acceptable.
UINT g_originalInputCp = 0;
UINT g_originalOutputCp = 0;

void RestoreConsoleCodePages() {
    if (g_originalInputCp != 0) SetConsoleCP(g_originalInputCp);
    if (g_originalOutputCp != 0) SetConsoleOutputCP(g_originalOutputCp);
}

void SetupConsoleUtf8() {
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
        g_originalInputCp = GetConsoleCP();
        SetConsoleCP(CP_UTF8);
    }
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
        g_originalOutputCp = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
    }
    std::atexit(RestoreConsoleCodePages);
}
} // namespace
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: nvm <program.ncu|.npkg> [-I <dir>...] "
                     "[--verbose | -v] [--gc-stress=N]\n"
                  << "       nvm --version\n";
        return 1;
    }

    //Version gate first: no runtime is initialized yet, so the plain
    //return is safe (the ExitProcess notes below only apply after
    //Runtime::StaticInit registers static destructors).
    if (std::string(argv[1]) == "--version") {
        std::cout << "nvm (NLang) " << NLANG_VERSION << "\n";
        return 0;
    }

#ifdef _WIN32
    //SetErrorMode first, then the crash reporter (CrashReporter.h contract)
    //so a crash during startup can't pop a WER dialog first.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    nlang::InstallCrashReporter("nvm");
    //Console UTF-8 — per-stream, console handles only (rationale in
    //SetupConsoleUtf8 above); see ncc's main for the manifest code
    //page companion.
    SetupConsoleUtf8();
#endif

    //0.7.5: OP_Prim_to_str formats scalars through the Rn builtin
    //type singletons; their construction interns names via IdString,
    //whose tables only exist after Runtime::StaticInit (a nullptr
    //s_pIndexMap dereference was the 0xC0000005 on the first to-string).
    Runtime::StaticInit();

    CompiledModule module;
    VmExecutor executor;
    //GC stress knob (testing): cap both GC thresholds so collection runs
    //at tiny population sizes — any untraced string handle goes stale
    //within a few allocations instead of surviving on the default
    //threshold. The flag parses from any position; the first non-flag
    //argument is the module.
    size_t gcStress = 0;
    std::vector<std::string> importDirs;
    bool verbose = false;
    int moduleArg = -1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--gc-stress=", 0) == 0) {
            //A silently-off knob can green a GC mutation matrix for the
            //wrong reason — diagnose any parse miss loudly instead.
            const char* digits = a.c_str() + 12;   //past "--gc-stress="
            errno = 0;
            char* parseEnd = nullptr;
            unsigned long parsed = std::strtoul(digits, &parseEnd, 10);
            if (digits[0] == '-' || parseEnd == digits
                    || *parseEnd != '\0' || errno == ERANGE) {
                std::fprintf(stderr,
                    "nvm: invalid --gc-stress value '%s' (flag ignored)\n",
                    digits);
                continue;
            }
            gcStress = static_cast<size_t>(parsed);
        } else if (a == "-I") {
            //Library search dir (native DLLs); a missing value is a mistake.
            if (i + 1 >= argc) {
                std::fprintf(stderr, "nvm: option -I requires a value\n");
                return 1;
            }
            importDirs.push_back(argv[++i]);
        } else if (a.size() > 2 && a.compare(0, 2, "-I") == 0) {
            importDirs.push_back(a.substr(2));
        } else if (a == "--verbose" || a == "-v") {
            //Print the resolved import search path, then run normally.
            verbose = true;
        } else if (moduleArg < 0) {
            moduleArg = i;
        }
    }
    if (moduleArg < 0) {
        std::cerr << "Usage: nvm <program.ncu|.npkg> [-I <dir>...] "
                     "[--verbose | -v] [--gc-stress=N]\n"
                  << "       nvm --version\n";
        return 1;
    }
    if (gcStress)
        executor.SetGcStressThresholds(gcStress);
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    int result = 1;
    try {
        //Phase 6: every artifact executes through the closure loader and
        //the load-time linker (nloader -> nlink). Artifacts are per-unit
        //images (a .ncu unit, or a .npkg whose members are its units) —
        //the loader walks the import slots to the whole closure and nlink
        //merges the peers; this call sequence never sees the difference.
        fs::path modPath(argv[moduleArg]);
        SearchPathInput search;
        search.explicitDirs = importDirs;
        search.baseDirs.push_back(modPath.parent_path().string());
        if (const char* env = std::getenv("NLANG_PATH"))
            search.pathEnv = env;
        search.systemDirs = {
            NativeLibraryLoader::ExecutableDir(), "."
        };
        //Unified library search: CLI -I > module dir > NLANG_PATH >
        //exe dir/cwd (native DLLs may ship beside the module or in -I
        //dirs), plus the system stdlib directory (where stdlib.npkg
        // lives) last. ONE list serves both the native search dirs and
        //the closure loader (ndb composes the same way). The layered
        //build keeps per-directory attribution for --verbose.
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
        for (const auto& d : allDirs)
            executor.AddNativeSearchDir(d);
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = std::move(allDirs);
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(argv[moduleArg], loaderOpts);
        module = NcuLinker::Link(loaded.units, loaded.entryKey);
        result = executor.Execute(module);
    } catch (const std::exception& e) {
        std::cerr << "Runtime error: " << e.what() << "\n";
        const auto& bt = executor.Backtrace();
        if (!bt.empty()) {
            std::cerr << "Backtrace:\n" << bt;
        }
#ifdef _WIN32
        ExitProcess(static_cast<UINT>(result));
#else
        return result;
#endif
    }
    //On Windows, static destructors from the runtime library can
    //corrupt the process exit code. ExitProcess() bypasses this.
#ifdef _WIN32
    ExitProcess(static_cast<UINT>(result));
#else
    return result;
#endif
}
