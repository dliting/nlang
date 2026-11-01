// --- crash reporter filter (shared by ncc/nvm/ndb/ndisasm mains) ---
// Body moved out of the CrashReporter.h inline (2026-09-27
// maintainability refactor, zero behavior change): the unhandled-
// exception filter plus its banner. Frame symbolization lives once in
// runtime/StackDump.cpp (PrintBacktrace) and is shared with NLANG_ASSERT's
// DumpStackTrace.

#ifdef _WIN32

#include "CrashReporter.h"
#include <nlang/runtime/StackDump.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace nlang {

//Set by InstallCrashReporter; the filter reads it via this static
//pointer (no capture — a filter is a plain function pointer).
static const char* g_programName = nullptr;

//Unhandled-exception filter: banner line plus (for non-stack-overflow
//crashes) a symbolized backtrace (PrintBacktrace). Everything on stack
//buffers — no heap allocation inside the filter (see header design
//notes).
static LONG CALLBACK NlangCrashFilter(EXCEPTION_POINTERS* ep) {
    unsigned excCode = (ep && ep->ExceptionRecord)
        ? ep->ExceptionRecord->ExceptionCode : 0;

    //For stack overflow, skip the backtrace — the filter runs on the
    //exhausted stack and SymInitialize + frame walking can blow it.
    bool isStackOverflow = (excCode == EXCEPTION_STACK_OVERFLOW);

    char banner[320];
    std::snprintf(banner, sizeof(banner),
        "%s: internal crash (code 0x%08X)%s, backtrace:\n",
        g_programName ? g_programName : "nlang",
        excCode,
        isStackOverflow ? " [stack overflow — symbols skipped]" : "");
    if (!isStackOverflow) {
        PrintBacktrace(banner);
    } else {
        //WriteFile bypasses the CRT heap — safe under heap corruption.
        HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
        WriteFile(herr, banner,
            static_cast<DWORD>(std::strlen(banner)), nullptr, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashReporter(const char* programName) {
    g_programName = programName;
    SetUnhandledExceptionFilter(NlangCrashFilter);
}

} // namespace nlang

#endif // _WIN32
