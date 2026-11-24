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
#include <nlang/runtime/StackDump.h>

//Flex entry point with the bison-bridge signature (defined in the
//generated nlang.lex.cpp; declared here at global scope -- an extern
//inside namespace nlang would name a different symbol).
int yylex(YYSTYPE* yylval_param, YYLTYPE* yylloc_param, void* yyscanner);

namespace nlang
{

ScriptScanner::ScriptScanner(ContextType ct):
	m_ContextType(ct), m_nStartState(0), m_pTransUnit(nullptr),
	m_pScanInfo(nullptr), m_pBuffer(nullptr), m_bDeferEof(false),
	m_nGenericDepth(0), m_bPendingGenericOpen(false),
	m_bPendingClassDeclName(false), m_bPendingPathSegment(false),
	m_bPendingReceiverLt(false)
{
}

ScriptScanner::~ScriptScanner()
{
	NLANG_ASSERT(!m_pScanInfo);
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

	//Keep the normalized text so the spelling probe can look past the
	//current token (see ProbeTypeArgumentOpener); freed on close.
	m_sScanText = std::move(normalized);
	BuildLineStarts();

	InitScanInfo();
	m_pBuffer = yy_scan_bytes(m_sScanText.data(),
		static_cast<int>(m_sScanText.size()), m_pScanInfo);
	NLANG_ASSERT(m_pBuffer);
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
	m_sScanText.clear();
	m_scanLineStarts.clear();
}

void ScriptScanner::OpenString(const char* szInput)
{
	//Same text-keeping contract as OpenFile: the spelling probe reads
	//past the current token through this copy.
	m_sScanText = szInput;
	BuildLineStarts();
	InitScanInfo();
	m_pBuffer = yy_scan_string(szInput, m_pScanInfo);
	NLANG_ASSERT(m_pBuffer);
	//Start a new line/column for each input string. flex initializes
	//line/column only for FILE buffers (yy_init_buffer); string buffers
	//come back with an uninitialized yy_bs_lineno, so set both here.
	ResetCol();
	yyset_lineno(1, m_pScanInfo);
}

void ScriptScanner::CloseString()
{
	FiniScanInfo();
	m_sScanText.clear();
	m_scanLineStarts.clear();
}

namespace
{

//Characters that cannot appear inside a type-argument list; hitting one
//aborts the probe so the '<' keeps its comparison reading.
bool IsTypeListForeignChar(char c)
{
	return c == '"' || c == '\'' || c == '(' || c == ')'
		|| c == '{' || c == '}' || c == ';';
}

//Skips whitespace and both comment forms from the offset; returns the
//next content offset (the text size at end of input).
size_t SkipBlanksAndComments(const std::string& text, size_t offset)
{
	const size_t size = text.size();
	while (offset < size)
	{
		const char c = text[offset];
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
		{
			++offset;
			continue;
		}
		if (c == '/' && offset + 1 < size && text[offset + 1] == '/')
		{
			while (offset < size && text[offset] != '\n')
				++offset;
			continue;
		}
		if (c == '/' && offset + 1 < size && text[offset + 1] == '*')
		{
			offset += 2;
			while (offset + 1 < size
				&& !(text[offset] == '*' && text[offset + 1] == '/'))
				++offset;
			offset = (offset + 2 < size) ? offset + 2 : size;
			continue;
		}
		break;
	}
	return offset;
}

//Bound on one probe's forward scan: a pathological open-ended '<'
//(unterminated comparison) must not walk the rest of the file.
const size_t MAX_TYPE_LIST_PROBE_CHARS = 4096;

//"not found" marker of ScanAngleBody (size_t can hold it, text sizes cannot).
constexpr size_t kAngleBodyNotFound = static_cast<size_t>(-1);

//Scans the balanced <...> body opened at \p opener. Returns the offset
//of the first byte after the closing '>', or kAngleBodyNotFound when
//the body runs out of input, exceeds the probe budget, or meets a
//character no type list may contain.
size_t ScanAngleBody(const std::string& text, size_t opener)
{
	size_t depth = 0;
	size_t i = opener;
	while (i < text.size() && i - opener <= MAX_TYPE_LIST_PROBE_CHARS)
	{
		i = SkipBlanksAndComments(text, i);
		if (i >= text.size())
			return kAngleBodyNotFound;
		const char c = text[i];
		if (c == '<')
		{
			++depth;
			++i;
		}
		else if (c == '>')
		{
			--depth;
			++i;
			if (depth == 0)
				return i;
		}
		else if (IsTypeListForeignChar(c))
			return kAngleBodyNotFound;
		else
			++i;
	}
	return kAngleBodyNotFound;
}

} //namespace

void ScriptScanner::BuildLineStarts()
{
	m_scanLineStarts.clear();
	m_scanLineStarts.push_back(0);
	for (size_t i = 0; i < m_sScanText.size(); ++i)
		if (m_sScanText[i] == '\n')
			m_scanLineStarts.push_back(i + 1);
}

//0.13.1c identity rescue: slice the retained text between two token
//locations. YY_USER_ACTION stores 1-based start columns and one-past
//end columns over the same byte grid the line-start table maps, so the
//conversion matches ProbeTypeArgumentOpener's probe position math.
std::string ScriptScanner::TextBetween(const ScriptLocation &begin,
	const ScriptLocation &end) const
{
	if (m_sScanText.empty() || m_ContextType != CT_Compiler
		|| begin.m_nStartLine == 0 || begin.m_nStartCol == 0
		|| begin.m_nStartLine > m_scanLineStarts.size()
		|| end.m_nEndLine == 0
		|| end.m_nEndLine > m_scanLineStarts.size())
		return {};
	const size_t beginOffset = m_scanLineStarts[begin.m_nStartLine - 1]
		+ begin.m_nStartCol - 1;
	const size_t endOffset = m_scanLineStarts[end.m_nEndLine - 1]
		+ end.m_nEndCol;
	if (beginOffset >= endOffset || endOffset > m_sScanText.size())
		return {};
	return m_sScanText.substr(beginOffset, endOffset - beginOffset);
}

void ScriptScanner::ProbeTypeArgumentOpener(size_t line, size_t column)
{
	m_bPendingReceiverLt = false;
	//0.13.1c identity rescue: forced comparison reading skips the shape
	//probe — the fragment span must re-read as plain comparisons.
	if (m_bForceComparisonReads)
		return;
	if (m_sScanText.empty() || m_ContextType != CT_Compiler
		|| line == 0 || line > m_scanLineStarts.size())
		return;
	//YY_USER_ACTION has advanced yycolumn past the identifier before the
	//rule calls this, so (line, column) is the first byte after it.
	const size_t afterToken = m_scanLineStarts[line - 1] + column;
	const size_t opener = SkipBlanksAndComments(m_sScanText, afterToken);
	if (opener >= m_sScanText.size() || m_sScanText[opener] != '<')
		return;
	const size_t afterBody = ScanAngleBody(m_sScanText, opener);
	if (afterBody == kAngleBodyNotFound)
		return;
	const size_t follow = SkipBlanksAndComments(m_sScanText, afterBody);
	if (follow < m_sScanText.size())
	{
		const char terminator = m_sScanText[follow];
		m_bPendingReceiverLt = (terminator == '.' || terminator == '(');
	}
}

void ScriptScanner::InitScanInfo()
{
	NLANG_ASSERT(!m_pScanInfo);
	m_nGenericDepth = 0;
	m_bPendingGenericOpen = false;
	m_bPendingClassDeclName = false;
	//Per-file clear: an illegal file ending on a bare '.' must not let
	//the segment arm leak into the next translation unit.
	m_bPendingPathSegment = false;
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
