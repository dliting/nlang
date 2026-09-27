// --- crash reporter filter (shared by ncc/nvm/ndb/ndisasm mains) ---
// Body moved out of the CrashReporter.h inline (2026-09-27
// maintainability refactor, zero behavior change): the unhandled-
// exception filter plus its per-frame symbolizer, each under the
// 50-line function limit.

#ifdef _WIN32

#include "CrashReporter.h"
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>

#pragma comment(lib, "dbghelp.lib")

namespace nlang {

//Set by InstallCrashReporter; the filter reads it via this static
//pointer (no capture — a filter is a plain function pointer).
static const char* g_programName = nullptr;

//Write one backtrace frame line: symbolized "name +0x displacement"
//when dbghelp resolves it, the raw address otherwise.
static void WriteFrameLine(HANDLE herr, WORD frameNo, void* frame) {
    char buf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    char line[320];
    int len;
    if (SymFromAddr(GetCurrentProcess(),
                    reinterpret_cast<DWORD64>(frame), &disp, sym)) {
        len = std::snprintf(line, sizeof(line), "  [%u] %s +0x%llX\n",
                            frameNo, sym->Name,
                            static_cast<unsigned long long>(disp));
    } else {
        len = std::snprintf(line, sizeof(line), "  [%u] 0x%p\n",
                            frameNo, frame);
    }
    if (len > 0)
        WriteFile(herr, line, static_cast<DWORD>(len), nullptr, nullptr);
}

//Unhandled-exception filter: banner line plus (for non-stack-overflow
//crashes) a symbolized backtrace. Everything on stack buffers — no
//heap allocation inside the filter (see header design notes).
static LONG CALLBACK NlangCrashFilter(EXCEPTION_POINTERS* ep) {
    unsigned excCode = (ep && ep->ExceptionRecord)
        ? ep->ExceptionRecord->ExceptionCode : 0;

    //For stack overflow, skip symbolization — the filter runs on the
    //exhausted stack and SymInitialize + frame walking can blow it.
    bool isStackOverflow = (excCode == EXCEPTION_STACK_OVERFLOW);

    char line[320];
    int len = std::snprintf(line, sizeof(line),
        "%s: internal crash (code 0x%08X)%s, backtrace:\n",
        g_programName ? g_programName : "nlang",
        excCode,
        isStackOverflow ? " [stack overflow — symbols skipped]" : "");

    //WriteFile bypasses CRT heap — safe under heap corruption.
    HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
    if (len > 0)
        WriteFile(herr, line, static_cast<DWORD>(len), nullptr, nullptr);

    if (!isStackOverflow) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        void* frames[64];
        WORD n = CaptureStackBackTrace(0, 64, frames, nullptr);
        for (WORD i = 0; i < n; ++i)
            WriteFrameLine(herr, i, frames[i]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashReporter(const char* programName) {
    g_programName = programName;
    SetUnhandledExceptionFilter(NlangCrashFilter);
}

} // namespace nlang

#endif // _WIN32
