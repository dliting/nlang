/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptScanner.cpp
	This file define the implementation of the lexical scanner for the nlang 
compiler.
-----------------------------------------------------------------------------*/

#include "ScriptScanner.h"
#include "TranslationUnit.h"
#include "nlang.tab.h"
#include "nlang.lex.h"
#include <nlang/runtime/Log.h>
#include <cassert>
#include <iosfwd>
#include <cstdio>

//Flex entry point with the bison-bridge signature (defined in the
//generated nlang.lex.cpp; declared here at global scope -- an extern
//inside namespace nlang would name a different symbol).
int yylex(YYSTYPE* yylval_param, YYLTYPE* yylloc_param, void* yyscanner);

namespace nlang
{

ScriptScanner::ScriptScanner(ContextType ct):
	m_ContextType(ct), m_nStartState(0), m_pTransUnit(nullptr),
	m_pScanInfo(nullptr), m_pBuffer(nullptr), m_bDeferEof(false),
	m_nGenericDepth(0), m_bPendingGenericOpen(false)
{
}

ScriptScanner::~ScriptScanner()
{
	assert(!m_pScanInfo);
}

bool ScriptScanner::OpenFile(const std::string& sFilePath)
{
	//TODO: use a portable method.
	FILE* file = fopen(sFilePath.c_str(), "r");
	if (!file)
	{
		LogError("Open nlang script file \"%s\" error %s.\n",
			sFilePath.c_str(), strerror(errno));
		return false;
	}
	m_pBuffer = file;

	InitScanInfo();
	yyrestart(file, m_pScanInfo);
	return true;
}

void ScriptScanner::CloseFile()
{
	fclose(yyget_in(m_pScanInfo));
	FiniScanInfo();
}

void ScriptScanner::OpenString(const char* szInput)
{
	InitScanInfo(); 
	m_pBuffer = yy_scan_string(szInput, m_pScanInfo);
	assert(m_pBuffer);
	//Start a new column for each input string.
	ResetCol();
}

void ScriptScanner::CloseString()
{
	FiniScanInfo();
}

void ScriptScanner::InitScanInfo()
{
	assert(!m_pScanInfo);
	m_nGenericDepth = 0;
	m_bPendingGenericOpen = false;
	if(yylex_init_extra(this, &m_pScanInfo))
		throw std::logic_error("yylex_init_extra() failed");
}

void ScriptScanner::FiniScanInfo()
{
	yylex_destroy(m_pScanInfo); 
	m_pScanInfo = nullptr; 
}

void ScriptScanner::ResetCol()
{
	yyset_column(0, m_pScanInfo);
	m_Location.m_nStartCol	= 1;
	m_Location.m_nEndCol	= 0;
}

int ScriptScanner::NextToken()
{
	//Editor-mode rules only write scalar yylval members, so the union
	//is scratch space here; locations land in m_Location for the caller.
	YYSTYPE tokenValue;
	return yylex(&tokenValue, &m_Location, m_pScanInfo);
}

} //namespace nlang
