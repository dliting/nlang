#include "ModuleLoader.h"
#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#include "nlang/common/LibrarySearchPath.h"
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: nvm <module.nmod> [-I <dir>...] [--gc-stress=N]\n"
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
#endif

    CompiledModule module;
    VmExecutor executor;
    //GC stress knob (testing): cap both GC thresholds so collection runs
    //at tiny population sizes — any untraced string handle goes stale
    //within a few allocations instead of surviving on the default
    //threshold. The flag parses from any position; the first non-flag
    //argument is the module.
    size_t gcStress = 0;
    std::vector<std::string> importDirs;
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
        } else if (moduleArg < 0) {
            moduleArg = i;
        }
    }
    if (moduleArg < 0) {
        std::cerr << "Usage: nvm <module.nmod> [-I <dir>...] [--gc-stress=N]\n"
                  << "       nvm --version\n";
        return 1;
    }
    if (gcStress)
        executor.SetGcStressThresholds(gcStress);
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    int result = 1;
    try {
        module = ModuleLoader::Load(argv[moduleArg]);
        //Unified library search: CLI -I > module dir > NLANG_PATH >
        //exe dir/cwd (native DLLs may ship beside the module or in -I dirs).
        fs::path modPath(argv[moduleArg]);
        SearchPathInput search;
        search.explicitDirs = importDirs;
        search.baseDirs.push_back(modPath.parent_path().string());
        if (const char* env = std::getenv("NLANG_PATH"))
            search.pathEnv = env;
        search.systemDirs = {
            NativeLibraryLoader::ExecutableDir(), "."
        };
        for (const auto& d : BuildLibrarySearchPath(search))
            executor.AddNativeSearchDir(d);
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
