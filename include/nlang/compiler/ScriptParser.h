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
class SnExpression;

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

	/*
	Identity-rescue support (0.13.1c): the verbatim source bytes between
	two token locations, read from the scanner's retained text (the
	resolver holds no source text). Empty when either position falls
	outside the currently open text.
	*/
	std::string CaptureSpanText(const ScriptLocation &begin,
		const ScriptLocation &end);

	void Log(CompileLogLevel, ISourceLocation&, const char *szFormat, ...);

	/*
	Parse a translation unit.
	\param unit The translation unit to be parsed.
	\param bEnableDebug Do we allow the parser output debug information?
	\param pSourceText When non-null, parse this in-memory text instead
	       of opening unit.FilePath() (the embedded prelude copy).
	\return false on error.
	*/
	bool ParseUnit(TranslationUnit&, bool bEnableDebug = false,
		const char* pSourceText = nullptr);

	/*
	Identity-rescue fragment parse (0.13.1c): re-read a misread span in
	forced comparison reading. The span is wrapped in a minimal unit
	(bare void function + target call) so the existing grammar drives it
	unchanged; diagnostics are quarantined in a silent logger -- only the
	AST matters, and any fragment failure is the caller's signal to fall
	back to its own diagnostic. On success the wrapper call's parsed
	arguments are detached into outArgs with every location remapped from
	fragment coordinates to the real file: spanStart is the misread
	head's absolute location, supplying the column shift, the line base,
	and the translation unit the spliced locations must name.
	\return true when the span parsed as an argument list (outArgs
	non-empty, ownership passed to the caller).
	*/
	static bool ParseFragment(const std::string &spanText,
		const ScriptLocation &spanStart,
		std::vector<SnExpression*> &outArgs);
private:
	BuildEnvironment &m_Env;
	ScriptScanner m_Scanner;
	TranslationUnit *m_pTransUnit;
};

}
