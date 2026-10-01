/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptScanner.cpp
	This file define the implementation of the lexical scanner for the nlang 
compiler.
-----------------------------------------------------------------------------*/

#include "ScriptScanner.h"
#include "TranslationUnit.h"
#include "nlang.tab.h"
#include "nlang.lex.h"
#include "nlang/compiler/Utf8.h"
#include <cassert>
#include <fstream>
#include <iterator>

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

//Line endings normalize to LF. CRLF pairs fold exactly like the old
//text-mode fopen did on Windows (now explicit and portable); a lone
//CR is deliberately treated as a line terminator too — a superset
//of the old behavior, so classic-Mac CR-only endings parse as lines
//instead of a syntax error.
static std::string NormalizeLineEndings(const char* content, size_t length)
{
	std::string normalized;
	normalized.reserve(length);
	for (size_t i = 0; i < length; ++i)
	{
		char c = content[i];
		if (c == '\r')
		{
			if (i + 1 < length && content[i + 1] == '\n')
				continue;  //CR of a CRLF pair — the LF below carries it
			normalized.push_back('\n');
			continue;
		}
		normalized.push_back(c);
	}
	return normalized;
}

bool ScriptScanner::OpenFile(const std::string& sFilePath)
{
	m_sOpenError.clear();
	//Read the raw bytes and enforce the UTF-8 input contract before any
	//tokenizing: a legacy-encoded source used to pass silently (mojibake
	//in string constants) and a UTF-8 BOM rode into the first token and
	//corrupted it. yy_scan_bytes copies the validated text, so `content`
	//may live on this stack frame.
	std::ifstream file(sFilePath.c_str(), std::ios::in | std::ios::binary);
	if (!file)
	{
		m_sOpenError = "Cannot open source file: " + sFilePath;
		return false;
	}
	std::string raw((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());

	const char* content = nullptr;
	size_t contentLength = 0;
	std::string reason;
	if (!Utf8ContentCheck(raw.data(), raw.size(),
		&content, &contentLength, &reason))
	{
		m_sOpenError = "Source file " + sFilePath + " " + reason
			+ ". Save the file as UTF-8.";
		return false;
	}

	std::string normalized = NormalizeLineEndings(content, contentLength);

	InitScanInfo();
	m_pBuffer = yy_scan_bytes(normalized.data(),
		static_cast<int>(normalized.size()), m_pScanInfo);
	assert(m_pBuffer);
	//Bytes buffers need the same explicit line/column init as string
	//buffers (flex initializes them only for FILE buffers).
	ResetCol();
	yyset_lineno(1, m_pScanInfo);
	return true;
}

void ScriptScanner::CloseFile()
{
	//The input is an yy_scan_bytes buffer, not a FILE*: yylex_destroy
	//inside FiniScanInfo frees it — there is no handle to fclose.
	FiniScanInfo();
}

void ScriptScanner::OpenString(const char* szInput)
{
	InitScanInfo();
	m_pBuffer = yy_scan_string(szInput, m_pScanInfo);
	assert(m_pBuffer);
	//Start a new line/column for each input string. flex initializes
	//line/column only for FILE buffers (yy_init_buffer); string buffers
	//come back with an uninitialized yy_bs_lineno, so set both here.
	ResetCol();
	yyset_lineno(1, m_pScanInfo);
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
