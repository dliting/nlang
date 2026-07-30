/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptParser.h
	This file define the interface of a script parser in the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "SyntaxNode.h"
#include "ScriptScanner.h"

namespace nlang
{

class BuildEnvironment;
class TranslationUnit;

/*
A source code parser to parse an nlang translation unit.
This parser internally uses a flex scanner and a bison parser. This parser can 
be reused to parse more than one translation unit.
*/
class NLANG_COMPILER_API ScriptParser
{
public:
	explicit ScriptParser(BuildEnvironment&);

	BuildEnvironment &Env()
	{
		return m_Env;
	}

	ScriptScanner& Scanner()
	{
		return m_Scanner;
	}

	TranslationUnit *TransUnit()
	{
		return m_pTransUnit;
	}

	void Log(CompileLogLevel, ISourceLocation&, const char *szFormat, ...);

	/*
	Parse a translation unit.
	\param unit The translation unit to be parsed.
	\param bEnableDebug Do we allow the parser output debug information?
	\return false on error.
	*/
	bool ParseUnit(TranslationUnit&, bool bEnableDebug = false);
private:
	BuildEnvironment &m_Env;
	ScriptScanner m_Scanner;
	TranslationUnit *m_pTransUnit;
};

}
