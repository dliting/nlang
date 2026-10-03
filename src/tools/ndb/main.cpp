#include "NcuLoader.h"
#include "NcuLinker.h"
#include "VmExecutor.h"
#include "NativeLibraryLoader.h"
#include "DebugSession.h"
#include "DebugSessionController.h"
#include "MachineFrontEnd.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#include "nlang/common/LibrarySearchPath.h"
#include "nlang/langservice/SymbolIndex.h"
#ifdef _WIN32
#include <windows.h>  //SetErrorMode/ExitProcess (was transitive via CrashReporter.h)
#endif
#include <nlang_version.h>  // generated from the repo VERSION file
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using namespace nlang;

//Assemble the unified, ordered search path for a debug run: CLI -I >
//module dir > NLANG_PATH > exe dir/cwd (native DLLs may ship beside the
//module or in -I dirs), plus the system stdlib directory last — package
//members and stdlib units resolve from the same dirs (phase 6 mirrors
//nvm's composition). One path serves both the native search dirs and
//the closure loader's options.
static std::vector<std::string> BuildDebugSearchPath(
    const std::string& modulePath, const std::vector<std::string>& cliDirs) {
    fs::path modPath(modulePath);
    SearchPathInput search;
    search.explicitDirs = cliDirs;
    search.baseDirs.push_back(modPath.parent_path().string());
    if (const char* env = std::getenv("NLANG_PATH"))
        search.pathEnv = env;
    search.systemDirs = {
        NativeLibraryLoader::ExecutableDir(), "."
    };
    std::vector<std::string> dirs = BuildLibrarySearchPath(search);
    const std::string stdlibDir = langservice::FindStdLibDir(
        NativeLibraryLoader::ExecutableDir());
    if (!stdlibDir.empty())
        dirs.push_back(stdlibDir);
    return dirs;
}

//Load a program through the closure loader and the load-time linker
//(nloader -> nlink, the same call sequence as nvm): unit-image
//artifacts carry import slots, and the debugger runs the linked
//closure, not a single self-contained module. The same search path
//feeds the executor's native-DLL dirs.
static CompiledModule LoadLinkedProgram(VmExecutor& executor,
    const std::string& modulePath,
    const std::vector<std::string>& cliDirs) {
    const std::vector<std::string> searchDirs =
        BuildDebugSearchPath(modulePath, cliDirs);
    for (const auto& d : searchDirs)
        executor.AddNativeSearchDir(d);
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = searchDirs;
    const NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modulePath, loaderOpts);
    return NcuLinker::Link(loaded.units, loaded.entryKey);
}

//Machine mode run: wire the protocol front end + controller, take
//pre-run commands until `run`, then execute once. The process exit code
//is the program's code; a failed module load or an uncaught NLang
//exception reports as an error event and yields 1 (stderr stays
//untouched — the CrashReporter's diagnostic channel).
static int RunMachine(const char* modulePath,
                      const std::vector<std::string>& cliDirs) {
    CompiledModule module;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface) — CLI parity.
    RegisterTestNatives(executor);
    MachineFrontEnd front(module, std::cin, std::cout);
    try {
        module = LoadLinkedProgram(executor, modulePath, cliDirs);
        DebugSessionController controller(module, front);
        front.SetController(&controller);
        executor.SetDebugHooks(&controller);
        executor.SetHostIo(&front);
        //hello goes out wired-but-idle; the prelude takes breakpoints.
        front.PumpUntilRun();
        const int code = executor.Execute(module);
        front.OnExited(code);
        return code;
    } catch (const std::exception& e) {
        //Two failure classes land here: a failed closure load or link —
        //thrown before PumpUntilRun, so the error event PRECEDES hello,
        //and machine clients must tolerate that ordering — and an
        //uncaught NLang throw during Execute. Both report message +
        //backtrace as one escaped error event (the CLI prints the same
        //parts to stderr).
        std::string report = e.what();
        const std::string& backtrace = executor.Backtrace();
        if (!backtrace.empty())
            report += "\n" + backtrace;
        front.OnRuntimeError(report);
        return 1;
    }
}

//Parse the command line: skip an optional leading "--machine", locate the
//module (first non-flag) and collect "-I <dir>" / "-I<dir>" library dirs.
//False on a trailing -I with no value or no module.
static bool ParseDebugArgs(int argc, char* argv[], bool machine,
                           std::vector<std::string>& dirs,
                           int& moduleIndex) {
    moduleIndex = -1;
    const int first = machine ? 2 : 1;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-I") {
            if (i + 1 >= argc)
                return false;
            dirs.push_back(argv[++i]);
        } else if (a.size() > 2 && a.compare(0, 2, "-I") == 0) {
            dirs.push_back(a.substr(2));
        } else if (moduleIndex < 0) {
            moduleIndex = i;
        }
    }
    return moduleIndex >= 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ndb <program.ncu|.npkg> [-I <dir>...]\n"
                  << "       ndb --machine <program.ncu|.npkg> [-I <dir>...]\n"
                  << "       ndb --version\n";
        return 1;
    }

    //Version gate first: no runtime is initialized yet, so the plain
    //return is safe (the ExitProcess notes below only apply after
    //Runtime::StaticInit registers static destructors).
    if (std::string(argv[1]) == "--version") {
        std::cout << "ndb (NLang) " << NLANG_VERSION << "\n";
        return 0;
    }

    //Machine mode: stdin/stdout are the IDE protocol channel — banners
    //and prompts are suppressed, program output travels as events.
    const bool machine = std::string(argv[1]) == "--machine";
    std::vector<std::string> cliDirs;
    int moduleIndex = -1;
    if (!ParseDebugArgs(argc, argv, machine, cliDirs, moduleIndex)) {
        std::cerr << (machine
            ? "Usage: ndb --machine <program.ncu|.npkg> [-I <dir>...]\n"
            : "Usage: ndb <program.ncu|.npkg> [-I <dir>...]\n");
        return 1;
    }

#ifdef _WIN32
    //SetErrorMode first, then the crash reporter (CrashReporter.h contract)
    //so a crash during startup can't pop a WER dialog first.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    nlang::InstallCrashReporter("ndb");
#endif

    if (machine) {
        const int code = RunMachine(argv[moduleIndex], cliDirs);
#ifdef _WIN32
        ExitProcess(static_cast<UINT>(code));
#else
        return code;
#endif
    }

    CompiledModule module;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    int result = 1;
    try {
        module = LoadLinkedProgram(executor, argv[moduleIndex], cliDirs);
        //Debug session: the controller owns breakpoints/step state; the
        //CLI session is its terminal adapter. Initial stop at the first
        //statement (gdb `start` behavior); the interactive loop runs
        //inside the frozen window (controller's WaitUntilResume).
        DebugSession session(module, argv[moduleIndex], std::cin, std::cout);
        DebugSessionController controller(module, session);
        session.SetController(&controller);
        executor.SetDebugHooks(&controller);
        result = executor.Execute(module);
        std::cout << "Program exited with code " << result << ".\n";
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
