/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptParser.h
	This file define the implementation of a script parser in the nlang 
compiler.
-----------------------------------------------------------------------------*/

#include "ScriptParser.h"
#include "TranslationUnit.h"
#include "BuildEnvironment.h"
#include <cassert>
#include <stdarg.h>

extern int yyparse(nlang::ScriptParser&, void* yyscanner);
extern int yydebug;

namespace nlang
{

ScriptParser::ScriptParser(BuildEnvironment &env): 
	m_Env(env), m_Scanner(ScriptScanner::CT_Compiler), m_pTransUnit(0)
{
}

bool ScriptParser::ParseUnit(TranslationUnit &unit, bool bEnableDebug /*= false*/)
{
	assert(m_Env.CurrModule());
	if (!m_Scanner.OpenFile(unit.FilePath()))
		return false;
	m_Scanner.StartState(0);
	m_Scanner.TransUnit(&unit);
	yydebug = bEnableDebug ? 1 : 0;

	m_pTransUnit = &unit;
	bool bSuccess = (yyparse(*this, m_Scanner.ScanInfo()) == 0 && m_Env.HasError());
	m_Scanner.CloseFile();
	m_pTransUnit = nullptr;

	return bSuccess;
}

void ScriptParser::Log(CompileLogLevel level, ISourceLocation &loc, 
	const char *szFormat, ...)
{
	va_list ap;
	va_start(ap, szFormat);
	m_Env.VLog(level, loc, szFormat, ap);
	va_end(ap);
}

} //namespace nlang
