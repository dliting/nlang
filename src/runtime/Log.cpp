#include "Log.h"
#include <cstdio>
#include <cstdarg>

namespace nlang
{

void WriteLog(FILE* fp, const char* szFmt, va_list& ap)
{
	vfprintf(fp, szFmt, ap);
}

void LogError(const char* szFmt, ...)
{
	va_list ap;
	va_start(ap, szFmt);
	WriteLog(stderr, szFmt, ap);
	va_end(ap);
}

void LogDebug(const char* szFmt, ...)
{
	va_list ap;
	va_start(ap, szFmt);
	WriteLog(stdout, szFmt, ap);
	va_end(ap);
}

} //namepsace nlang