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
#include <nlang/runtime/StackDump.h>
#include <nlang/runtime/Module.h>

extern int yyparse(nlang::ScriptParser&, void* yyscanner);
extern int yydebug;

namespace nlang
{

namespace
{

//Identity-rescue fragment parse (0.13.1c): quarantines every fragment
//diagnostic. Counts still increment (CompileLogger::Log counts before
//WriteLog), so HasError() gates the rescue, but nothing reaches the
//user's log — a failed fragment falls back to the caller's own
//0.13.1b diagnostic.
class SilentLogger : public CompileLogger
{
	void WriteLog(CompileLogLevel, const ISourceLocation*,
		const char*) override
	{
	}
};

//Shifts every location of a rescued subtree from fragment coordinates
//to the real file. The wrapper prefix is single-line ASCII, so line 1
//columns shift by the prefix length and later lines carry over
//unchanged; each location also re-points at the real translation unit
//(the scratch one dies with this parse).
void RemapFragmentLocations(SyntaxNode &node, const ScriptLocation &spanStart,
	size_t prefixLen)
{
	if (node.Location())
	{
		auto *pLoc = const_cast<ScriptLocation *>(
			static_cast<const ScriptLocation *>(node.Location()));
		auto shift = [&spanStart, prefixLen](size_t &nLine, size_t &nCol)
		{
			if (nLine == 1)
			{
				NLANG_ASSERT(nCol > prefixLen);
				nCol += spanStart.m_nStartCol - 1 - prefixLen;
				nLine = spanStart.m_nStartLine;
			}
			else
			{
				nLine += spanStart.m_nStartLine - 1;
			}
		};
		shift(pLoc->m_nStartLine, pLoc->m_nStartCol);
		shift(pLoc->m_nEndLine, pLoc->m_nEndCol);
		pLoc->m_pTransUnit = spanStart.m_pTransUnit;
		//ToString prefers the snapshot name over the unit pointer, and the
		//fragment mints snapshot the scratch file — re-point at the real
		//file or every spliced diagnostic would name __rescue_span.n.
		pLoc->m_sFileName = spanStart.m_sFileName;
	}
	for (auto &child : node.Children())
		RemapFragmentLocations(static_cast<SyntaxNode &>(child),
			spanStart, prefixLen);
}

//The wrapper source mints exactly one call of this name.
SnInvokeExpr *FindRescueTargetCall(SyntaxNode *pNode)
{
	if (!pNode)
		return nullptr;
	if (pNode->Kind() == NK_InvokeExpr
		&& static_cast<SnInvokeExpr *>(pNode)->CalleeName()
			== "__rescue_target")
		return static_cast<SnInvokeExpr *>(pNode);
	for (auto &child : pNode->Children())
	{
		if (auto *pFound = FindRescueTargetCall(
				static_cast<SyntaxNode *>(&child)))
			return pFound;
	}
	return nullptr;
}

//The wrapper source around the re-parsed span: a bare void function
//whose body is one call, so the existing grammar drives it unchanged.
const char *const kWrapperHead = "void __rescue_span() { __rescue_target(";
const char *const kWrapperTail = "); }";

//Detaches the wrapper call's arguments (ownership passes to the caller)
//and remaps their locations into the real file. Pointers are collected
//first — detaching mid-iteration would walk a list being mutated.
void DetachRescuedArgs(SnInvokeExpr &call, const ScriptLocation &spanStart,
	std::vector<SnExpression*> &outArgs)
{
	std::vector<SnExpression*> args;
	for (auto &arg : call.Params())
		args.push_back(&arg);
	for (auto *pArg : args)
		call.DetachChild(pArg);
	const size_t prefixLen =
		std::char_traits<char>::length(kWrapperHead);
	for (auto *pArg : args)
	{
		RemapFragmentLocations(*pArg, spanStart, prefixLen);
		outArgs.push_back(pArg);
	}
}

} //namespace

ScriptParser::ScriptParser(BuildEnvironment &env): 
	m_Env(env), m_Scanner(ScriptScanner::CT_Compiler), m_pTransUnit(0)
{
}

bool ScriptParser::ParseUnit(TranslationUnit &unit,
	bool bEnableDebug /*= false*/, const char* pSourceText /*= nullptr*/)
{
	NLANG_ASSERT(m_Env.CurrModule());
	if (pSourceText == nullptr)
	{
		if (!m_Scanner.OpenFile(unit.FilePath()))
		{
			//OpenFile puts its full diagnosis (open failure or UTF-8 input
			//contract violation) in OpenError(); log through env so HasError()
			//gates downstream and the build aborts instead of segfaulting in
			//MergeTransUnits on a null TranslationUnit::Root().
			m_Env.Log(CLL_Error, "%s", m_Scanner.OpenError().c_str());
			return false;
		}
	}
	else
	{
		//In-memory source (the embedded prelude copy): the same scanner
		//pipeline over a string buffer.
		m_Scanner.OpenString(pSourceText);
	}
	m_Scanner.StartState(0);
	m_Scanner.TransUnit(&unit);
	yydebug = bEnableDebug ? 1 : 0;

	m_pTransUnit = &unit;
	bool bSuccess = (yyparse(*this, m_Scanner.ScanInfo()) == 0 && !m_Env.HasError());
	if (pSourceText == nullptr)
		m_Scanner.CloseFile();
	else
		m_Scanner.CloseString();
	m_pTransUnit = nullptr;

	return bSuccess;
}

bool ScriptParser::ParseFragment(const std::string &spanText,
	const ScriptLocation &spanStart,
	std::vector<SnExpression*> &outArgs)
{
	const std::string sSource =
		std::string(kWrapperHead) + spanText + kWrapperTail;

	//Scratch environment: no stdlib index, silent logger, an unregistered
	//module only because ParseUnit asserts a current module. Nothing here
	//touches the real build's registry or library index.
	BuildParams params;
	SilentLogger silentLogger;
	BuildEnvironment scratchEnv(params, silentLogger);
	std::unique_ptr<Module> upModule(
		ModuleManager::CreateUnregistered("__rescue_span"));
	if (!upModule)
		return false;
	Module *pScratchModule = upModule.get();
	scratchEnv.OwnModule(std::move(upModule));
	scratchEnv.CurrModule(*pScratchModule);

	ScriptParser fragParser(scratchEnv);
	fragParser.Scanner().SetForceComparisonReads(true);
	TranslationUnit scratchUnit("__rescue_span.n");
	if (!fragParser.ParseUnit(scratchUnit, false, sSource.c_str())
		|| scratchEnv.HasError())
		return false;

	//Detach the wrapper call's arguments (ownership passes to the
	//caller) and remap their locations into the real file.
	auto *pCall = FindRescueTargetCall(scratchUnit.Root());
	if (!pCall)
		return false;
	DetachRescuedArgs(*pCall, spanStart, outArgs);
	return !outArgs.empty();
}

std::string ScriptParser::CaptureSpanText(const ScriptLocation &begin,
	const ScriptLocation &end)
{
	return m_Scanner.TextBetween(begin, end);
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
