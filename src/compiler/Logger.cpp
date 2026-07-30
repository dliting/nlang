/*-----------------------------------------------------------------------------
	ncomp/intf/Logger.cpp
	This file define the implementation of compile logs.
-----------------------------------------------------------------------------*/

#include "Logger.h"
#include <ostream>
#include <cassert>
#include <iosfwd>
#include <sstream>
#include <cstdarg>
#include <cstdio>

namespace nlang
{

struct MakeLogHeader
{
	MakeLogHeader(CompileLogLevel level, 
		const ISourceLocation* pLocation):
		m_Level(level), m_pLocation(pLocation)
	{
	}

	const CompileLogLevel m_Level;
	const ISourceLocation* m_pLocation;
};

std::ostream& operator<<(std::ostream& os, const MakeLogHeader& maker)
{
	if (maker.m_Level == CLL_More)
		os << '\t';
	bool bEmptyHeader = true;
	if (maker.m_pLocation)
	{
		const std::string sLocation = maker.m_pLocation->ToString();
		if (!sLocation.empty())
			os << sLocation << ": ";
	}
	const char* szLevelName = CompileLogger::LOG_LEVEL_NAMES[maker.m_Level];
	if (szLevelName[0] != '\0')
		os << szLevelName << ": ";
	return os;
}

const char* CompileLogger::LOG_LEVEL_NAMES[CLL_COUNT] =
{
	"", //info
	"Warning",
	"Error",
	"Fatal error",
	"" //more
};

void CompileLogger::Log(CompileLogLevel level, const char* szFormat, ...)
{
	va_list ap;
	va_start(ap, szFormat);
	VLog(level, nullptr, szFormat, ap);
	va_end(ap);
}

void CompileLogger::Log(CompileLogLevel level, 
	const ISourceLocation* pLocation, const char* szFormat, ...)
{
	va_list ap;
	va_start(ap, szFormat);
	VLog(level, pLocation, szFormat, ap);
	va_end(ap);
}

void CompileLogger::VLog(CompileLogLevel level, 
	const ISourceLocation* pLocation, const char* szFormat, va_list& ap)
{
	assert(szFormat);
	static const size_t MAX_MSG_LENGTH = 1024;
	char szMessage[MAX_MSG_LENGTH];
	vsnprintf(szMessage, MAX_MSG_LENGTH, szFormat, ap);
	szMessage[MAX_MSG_LENGTH - 1] = '\0';
	WriteLog(level, pLocation, szMessage);
	if (level == CLL_Warn)
		m_nWarnings++;
	else if (level == CLL_Error)
		m_nErrors++;
	else if (level == CLL_Fatal)
		m_nFatals++;
}

CompileLogItem::CompileLogItem(CompileLogLevel level,
	ISourceLocation* pLocation, const char* szMessage) :
	m_Level(level),
	m_pLocation(pLocation),
	m_upMessage(std::make_unique<std::string>(szMessage))
{
}

CompileLogItem::~CompileLogItem()
{
	delete m_pLocation;
	// m_upMessage is now unique_ptr - auto-deleted
}

std::string CompileLogItem::FullMessage() const
{
	assert(m_Level < CLL_COUNT);
	std::stringstream ss;
	ss << MakeLogHeader(m_Level, m_pLocation) << *m_upMessage;
	return ss.str();
}

ListCompileLogger::ListCompileLogger() :
	m_upItems(new ItemList())
{
}

ListCompileLogger::~ListCompileLogger()
{
	clear();
	// m_upItems is now unique_ptr - auto-deleted
}

void ListCompileLogger::clear()
{
	for (auto pItem : *m_upItems)
		delete pItem;
	m_upItems->clear();
}

void ListCompileLogger::WriteLog(CompileLogLevel level,
	const ISourceLocation* pLocation, const char* szMessage)
{
	ISourceLocation* pLocCopy = pLocation ? pLocation->Clone().release() : nullptr;
	CompileLogItem* pItem = new CompileLogItem(level, pLocCopy, szMessage);
	m_upItems->push_back(pItem);
}


StreamCompileLogger::StreamCompileLogger(std::ostream& os): m_Stream(os)
{
}

void StreamCompileLogger::WriteLog(CompileLogLevel level, 
	const ISourceLocation* pLocation, const char* szMessage)
{
	m_Stream << MakeLogHeader(level, pLocation) << szMessage << std::endl;
}

} //namespace nlang
