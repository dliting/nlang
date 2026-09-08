#include "ModuleLoader.h"
#include "VmExecutor.h"
#include "DebugSession.h"
#include "DebugSessionController.h"
#include "TestNatives.h"
#include "CrashReporter.h"
#include <nlang_version.h>  // generated from the repo VERSION file
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include <cstdio>
#include <iostream>
#include <string>

using namespace nlang;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ndb <module.nmod>\n"
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

#ifdef _WIN32
    //SetErrorMode first, then the crash reporter (CrashReporter.h contract)
    //so a crash during startup can't pop a WER dialog first.
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    nlang::InstallCrashReporter("ndb");
#endif

    CompiledModule module;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    int result = 1;
    try {
        module = ModuleLoader::Load(argv[1]);
        //Debug session: the controller owns breakpoints/step state; the
        //CLI session is its terminal adapter. Initial stop at the first
        //statement (gdb `start` behavior); the interactive loop runs
        //inside the frozen window (controller's WaitUntilResume).
        DebugSession session(module, argv[1], std::cin, std::cout);
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
