#include "ModuleLoader.h"
#include "VmExecutor.h"
#include "DebugSession.h"
#include "DebugSessionController.h"
#include "MachineFrontEnd.h"
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

//Machine mode run: wire the protocol front end + controller, take
//pre-run commands until `run`, then execute once. The process exit code
//is the program's code; a failed module load or an uncaught NLang
//exception reports as an error event and yields 1 (stderr stays
//untouched — the CrashReporter's diagnostic channel).
static int RunMachine(const char* modulePath) {
    CompiledModule module;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface) — CLI parity.
    RegisterTestNatives(executor);
    MachineFrontEnd front(module, std::cin, std::cout);
    try {
        module = ModuleLoader::Load(modulePath);
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
        //Two failure classes land here: a failed ModuleLoader::Load —
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ndb <module.nmod>\n"
                  << "       ndb --machine <module.nmod>\n"
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
    if (machine && argc < 3) {
        std::cerr << "Usage: ndb --machine <module.nmod>\n";
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
        const int code = RunMachine(argv[2]);
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
