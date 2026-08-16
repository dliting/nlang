#pragma once
//Phase 9f audit: shared crash reporter for ncc/nvm. Extracted from the
//verbatim-duplicated ReportCrash in both mains so that fixes (e.g. replacing
//iostream with WriteFile to avoid CRT heap-lock deadlock) land once.
//
//Design notes:
//  - Uses WriteFile(GetStdHandle(STD_ERROR_HANDLE), ...) instead of
//    std::cerr: iostream allocates and takes the stream lock; heap-corruption
//    crashes typically occur with the CRT heap lock already held, so cerr
//    deadlocks instead of reporting. WriteFile is a kernel call that bypasses
//    the CRT heap entirely.
//  - Skips SymInitialize/SymFromAddr for EXCEPTION_STACK_OVERFLOW: the
//    filter runs on the exhausted stack, and module enumeration plus 64
//    captured frames plus symbolization can blow the remaining stack,
//    producing a second fault and process death with no output.
//  - All formatting uses stack buffers (snprintf into char[]) — no heap
//    allocation inside the filter.

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace nlang {

//Install the crash reporter as the unhandled-exception filter.
//Call once at program startup (after SetErrorMode, before any work).
inline void InstallCrashReporter(const char* programName) {
    //Thread-local: the lambda captures programName via a static pointer.
    static const char* s_programName = programName;
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* ep) -> LONG {
        unsigned excCode = (ep && ep->ExceptionRecord)
            ? ep->ExceptionRecord->ExceptionCode : 0;

        //For stack overflow, skip symbolization — the filter runs on the
        //exhausted stack and SymInitialize + frame walking can blow it.
        bool isStackOverflow = (excCode == EXCEPTION_STACK_OVERFLOW);

        char line[320];
        int len = std::snprintf(line, sizeof(line),
            "%s: internal crash (code 0x%08X)%s, backtrace:\n",
            s_programName ? s_programName : "nlang",
            excCode,
            isStackOverflow ? " [stack overflow — symbols skipped]" : "");

        //WriteFile bypasses CRT heap — safe under heap corruption.
        HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
        DWORD written = 0;
        if (len > 0)
            WriteFile(herr, line, static_cast<DWORD>(len), &written, nullptr);

        if (!isStackOverflow) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);

            void* frames[64];
            WORD n = CaptureStackBackTrace(0, 64, frames, nullptr);
            for (WORD i = 0; i < n; ++i) {
                char buf[sizeof(SYMBOL_INFO) + 256] = {};
                auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(GetCurrentProcess(),
                                reinterpret_cast<DWORD64>(frames[i]),
                                &disp, sym)) {
                    len = std::snprintf(line, sizeof(line),
                        "  [%u] %s +0x%llX\n",
                        static_cast<unsigned>(i), sym->Name,
                        static_cast<unsigned long long>(disp));
                } else {
                    len = std::snprintf(line, sizeof(line),
                        "  [%u] 0x%p\n",
                        static_cast<unsigned>(i), frames[i]);
                }
                if (len > 0)
                    WriteFile(herr, line, static_cast<DWORD>(len),
                              &written, nullptr);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
}

} // namespace nlang

#endif // _WIN32
