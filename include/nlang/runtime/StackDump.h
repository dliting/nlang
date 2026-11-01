/*-----------------------------------------------------------------------------
	nlang/runtime/StackDump.h
	This file defines the diagnostic assert and the stack-dump helpers
	shared by every nlang library, tool and public header.
-----------------------------------------------------------------------------*/
#pragma once
#include <cassert>

namespace nlang
{

//Write one symbolized frame line per captured frame of the CALLING
//thread to stderr (see StackDump.cpp for the CRT-free writing
//discipline). PrintBacktrace adds a banner line ahead of the frames.
void DumpStackTrace(const char* context);
void PrintBacktrace(const char* banner);

} //namespace nlang

#define NLANG_STRINGIZE_IMPL(x) #x
#define NLANG_STRINGIZE(x) NLANG_STRINGIZE_IMPL(x)

#ifdef NDEBUG
//Release builds: exactly the built-in assert (compiled out), so the
//macro can later be switched independently if performance ever needs
//a cheaper shape.
#define NLANG_ASSERT(cond) assert(cond)
#elif defined(_WIN32)
//Debug assert with a symbolized stack dump: the CRT assert prints no
//stack, and its abort bypasses the unhandled-exception filter, so an
//assert-only failure would otherwise leave no trace of its call path.
//The condition is evaluated twice only on the failing path (dump, then
//the CRT assert's own report) — keep NLANG_ASSERT conditions pure.
#define NLANG_ASSERT(cond)                                                   \
	do                                                                       \
	{                                                                        \
		if (!(cond))                                                         \
			nlang::DumpStackTrace("assert failed: " #cond " @" __FILE__      \
				":" NLANG_STRINGIZE(__LINE__));                              \
		assert(cond);                                                        \
	} while (0)
#else
//Debug on other platforms: the built-in assert unchanged (no stack
//dump channel there yet).
#define NLANG_ASSERT(cond) assert(cond)
#endif
