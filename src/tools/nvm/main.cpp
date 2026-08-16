#include "ModuleLoader.h"
#include "VmExecutor.h"
#include "TestNatives.h"
#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#include <dbghelp.h>
#endif
#include <cstdio>
#include <iostream>
#include <string>

using namespace nlang;

#ifdef _WIN32
//Crash reporter (see ncc main.cpp for rationale): symbolized backtrace
//on stderr instead of a silent native-crash exit.
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI ReportCrash(EXCEPTION_POINTERS* ep) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    void* frames[64];
    WORD n = CaptureStackBackTrace(0, 64, frames, nullptr);
    char line[320];
    unsigned excCode = (ep && ep->ExceptionRecord)
        ? ep->ExceptionRecord->ExceptionCode : 0;
    std::snprintf(line, sizeof(line),
        "nvm: internal crash (code 0x%08X), backtrace:\n", excCode);
    std::cerr << line;
    for (WORD i = 0; i < n; ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(),
                        reinterpret_cast<DWORD64>(frames[i]), &disp, sym)) {
            std::snprintf(line, sizeof(line), "  [%u] %s +0x%llX\n",
                static_cast<unsigned>(i), sym->Name,
                static_cast<unsigned long long>(disp));
        } else {
            std::snprintf(line, sizeof(line), "  [%u] 0x%p\n",
                static_cast<unsigned>(i), frames[i]);
        }
        std::cerr << line;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: nvm <module.nmod>\n";
        return 1;
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(&ReportCrash);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    CompiledModule module;
    VmExecutor executor;
    //Phase 9f: host-provided natives (e2e test surface).
    RegisterTestNatives(executor);
    int result = 1;
    try {
        module = ModuleLoader::Load(argv[1]);
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
