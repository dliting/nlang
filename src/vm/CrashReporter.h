#pragma once
//Phase 9f audit: shared crash reporter for ncc/nvm. Extracted from the
//verbatim-duplicated ReportCrash in both mains so that fixes (e.g. replacing
//iostream with WriteFile to avoid CRT heap-lock deadlock) land once.
//
//Design notes (implementation in CrashReporter.cpp):
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

namespace nlang {

//Install the crash reporter as the unhandled-exception filter.
//Call once at program startup (after SetErrorMode, before any work).
void InstallCrashReporter(const char* programName);

} // namespace nlang

#endif // _WIN32
