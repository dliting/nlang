/*---
    StackDump.cpp — the single implementation of the symbolized stack
    dump (DumpStackTrace/PrintBacktrace; CrashReporter's unhandled-
    exception filter reuses the same frame printing through
    PrintBacktrace). All writing goes through WriteFile (bypasses the
    CRT heap) so the dump also works under heap corruption.
---*/
#ifdef _WIN32

#include <nlang/runtime/StackDump.h>
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

namespace nlang
{

//Deferred-load symbol options, initialized once per process; repeated
//SymInitialize calls are tolerated but this keeps every dump cheap.
static void InitProcessSymbols()
{
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
}

//Write one backtrace frame line: symbolized "name +0x displacement"
//when dbghelp resolves it, the raw address otherwise.
static void WriteFrameLine(HANDLE herr, WORD frameNo, void* frame)
{
	char buf[sizeof(SYMBOL_INFO) + 256] = {};
	auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen = 255;
	DWORD64 disp = 0;
	char line[320];
	int len;
	if (SymFromAddr(GetCurrentProcess(),
			reinterpret_cast<DWORD64>(frame), &disp, sym))
	{
		len = std::snprintf(line, sizeof(line), "  [%u] %s +0x%llX\n",
			frameNo, sym->Name,
			static_cast<unsigned long long>(disp));
	}
	else
	{
		len = std::snprintf(line, sizeof(line), "  [%u] 0x%p\n",
			frameNo, frame);
	}
	if (len > 0)
		WriteFile(herr, line, static_cast<DWORD>(len), nullptr, nullptr);
}

//Shared body: banner line first (already formatted by the caller), then
//the symbolized frames of the calling thread. All stack buffers — no
//heap allocation on this path.
static void WriteBannerAndFrames(const char* banner)
{
	InitProcessSymbols();
	void* frames[64];
	WORD count = CaptureStackBackTrace(0, 64, frames, nullptr);
	HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
	if (banner && banner[0])
		WriteFile(herr, banner,
			static_cast<DWORD>(std::strlen(banner)), nullptr, nullptr);
	for (WORD i = 0; i < count; ++i)
		WriteFrameLine(herr, i, frames[i]);
}

void DumpStackTrace(const char* context)
{
	char banner[256];
	std::snprintf(banner, sizeof(banner), "%s: stack dump:\n",
		context ? context : "nlang");
	WriteBannerAndFrames(banner);
}

void PrintBacktrace(const char* banner)
{
	WriteBannerAndFrames(banner ? banner : "");
}

} //namespace nlang

#endif //_WIN32
