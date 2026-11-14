/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptScanner.h
	This file define the interface of a lexical scanner for the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "ScriptLocation.h"
#include <vector>

namespace nlang
{

class TranslationUnit;
/*
The code scanner for nlang script. 
	A scanner can be use either in a editor for syntax highlighting or in a
compiler for lexical analysis. 
	This class is a wrapper of a flex scanner.
	We can get the next token type by calling the method \a NextToken();
*/
class NLANG_COMPILER_API ScriptScanner
{
public:
	//The using context of a script scanner.
	enum ContextType 
	{
		CT_Editor,	//scanner used in an editor
		CT_Compiler	//scanner used in an compiler
	};
public:
	explicit ScriptScanner(ContextType);

	~ScriptScanner();

	//Open a script file to scan. The file must satisfy the UTF-8 input
	//contract (a leading UTF-8 BOM is accepted and skipped; invalid
	//bytes and UTF-16 saves are rejected) — on failure the diagnosis
	//is available through OpenError().
	bool OpenFile(const std::string& FilePath);

	//Why the last OpenFile() failed, empty when it succeeded (or was
	//never called). The sentence names the file and the remedy.
	const std::string& OpenError() const
	{
		return m_sOpenError;
	}

	//Close the file that has opened by \a OpenFile().
	void CloseFile();

	//Using a bytes string as the input stream to scan.
	void OpenString(const char* zInput);

	//Close the input string opened by \a OpenString().
	void CloseString();

	/*
	Get the next token (editor mode: after OpenString()). The compiler
	path drives yylex directly through the parser, so this wrapper is
	what makes the scanner usable stand-alone, e.g. for highlighting.
	\return a value of yytokentype.
	If the return value is 0, it means that we have reached end of the input
	stream.
	*/
	int NextToken();

	//The location of the token last returned by NextToken(). Columns are
	//1-based byte columns of the input given to OpenString()/OpenFile().
	const ScriptLocation& Location() const
	{
		return m_Location;
	}

	//The verbatim source bytes between two token locations (1-based
	//start, one-past end, as the location macros store them). Empty when
	//either position falls outside the text currently open. Serves the
	//0.13.1c identity rescue, which re-parses a misread span verbatim.
	std::string TextBetween(const ScriptLocation &begin,
		const ScriptLocation &end) const;

	//The scan information for flex internal use.
	void* ScanInfo() const
	{
		return m_pScanInfo;
	}

	//Get the translation unit.
	TranslationUnit* TransUnit() const
	{
		return m_pTransUnit;
	}

	//Set the translation unit.
	void TransUnit(TranslationUnit* pUnit)
	{
		m_pTransUnit = pUnit;
	}

	//Get the start state of flex.
	int StartState() const
	{
		return m_nStartState;
	}

	//Set the start state of flex.
	void StartState(int s)
	{
		m_nStartState = s;
	}

	//Get the scanner type.
	ContextType ScanType() const
	{
		return m_ContextType;
	}

	//Set the column of the source location to the initial position.
	void ResetCol();

	bool DeferEof() const
	{
		return m_bDeferEof;
	}

	void DeferEof(bool bShouldDefer)
	{
		m_bDeferEof = bShouldDefer;
	}

	//Current type-argument nesting depth. Incremented by '<' right after
	//a built-in generic name (List/Dict by the identifier rule; func by
	//its keyword rule), decremented by each '>'. While positive, ">>" is
	//split into two '>' tokens so nested generics like
	//`List<List<int>>` parse (C#-style scanner split).
	int GenericDepth() const
	{
		return m_nGenericDepth;
	}

	void IncGenericDepth()
	{
		++m_nGenericDepth;
	}

	void DecGenericDepth()
	{
		if (m_nGenericDepth > 0)
			--m_nGenericDepth;
	}

	//Resets the type-argument depth to zero. Called from the grammar's
	//statement-level error recovery: a syntax error inside an unclosed
	//type-argument list would otherwise leave the depth armed and
	//mis-split a later ">>" in the same translation unit.
	void ClearGenericDepth()
	{
		m_nGenericDepth = 0;
	}

	//True when the previous token was the identifier "List"/"Dict" or
	//the func keyword. Consumed (and cleared) by the '<' rule; set by
	//the identifier rule and the func keyword rule. The identity-rescue
	//fragment parse (0.13.1c) suppresses the arming entirely: its span
	//must re-read as plain comparisons.
	void PendingGenericOpen(bool bPending)
	{
		if (!m_bForceComparisonReads)
			m_bPendingGenericOpen = bPending;
	}

	//0.13.1c identity rescue: while set, generic-open arming (both the
	//builtin-name flag above and the spelling probe) is suppressed so
	//every '<' lexes as the comparison opener. Only the fragment parse
	//of a rescued span sets it, on a fresh scanner; in-class init keeps
	//OpenString's InitScanInfo from clearing a set flag.
	void SetForceComparisonReads(bool bForce)
	{
		m_bForceComparisonReads = bForce;
	}

	bool TakePendingGenericOpen()
	{
		bool bPending = m_bPendingGenericOpen;
		m_bPendingGenericOpen = false;
		return bPending;
	}

	//One-shot flag: the previous token was the "class" keyword. Taken
	//(and cleared) by the identifier rule — the class NAME position does
	//not arm the generic-open flag, so the generic-parameter brackets of
	//a prelude declaration (`class List<T>`) lex as plain '<'/'>' while
	//every other occurrence of the name keeps the OT_TypeLt reading.
	void MarkClassDeclHead()
	{
		m_bPendingClassDeclName = true;
	}

	bool TakePendingClassDeclName()
	{
		bool bPending = m_bPendingClassDeclName;
		m_bPendingClassDeclName = false;
		return bPending;
	}

	//0.13.1a Task 3: expression-position generic spelling, resolved with
	//the C# "grammar ambiguities" rule (ECMA-334 §7.6.4.2): the
	//disambiguation is a SHAPE rule, no identifier case test and no
	//symbol table. Declared generic names (Pair, Hold) cannot live in
	//the scanner's flag set, and in expression position a plain '<'
	//after an identifier is genuinely ambiguous (comparison vs
	//spelling) — no parser state can split it. So an identifier that is
	//immediately followed by a balanced <...> whose terminator is '.'
	//or '(' (exactly the two shapes the grammar creates: static member
	//call, constructor call) arms a one-shot flag and the following '<'
	//arrives as OT_TypeLt, distinct from the comparison opener by
	//construction; a call whose arguments are comparisons colliding
	//with the shape — `f(a < b, c > (d))` — reads as the spelling too
	//(C# behaves the same and expects the arguments parenthesized),
	//and the resolver names that situation when the head resolves to a
	//value (see TryResolveDeclaredGenericInstance). The reset below
	//stays unconditional: every identifier rewrites the flag, so a
	//stale arm cannot cross another identifier. The probe is a pure
	//character scan over the normalized source (kept by
	//OpenFile/OpenString for this purpose); no flex state is touched,
	//and unterminated or foreign-char shapes (quotes, parens, braces
	//inside the angles) leave the comparison reading intact. Inert in
	//editor mode.
	void ProbeTypeArgumentOpener(size_t line, size_t column);

	bool TakePendingReceiverLt()
	{
		bool bPending = m_bPendingReceiverLt;
		m_bPendingReceiverLt = false;
		return bPending;
	}
private:
	void InitScanInfo();

	void FiniScanInfo();

	//Rebuilds the line-start index over the kept scan text (see
	//m_sScanText).
	void BuildLineStarts();

	//The current token location.
	ScriptLocation m_Location;

	//Diagnosis of the last OpenFile() failure (see OpenError()).
	std::string m_sOpenError;

	ContextType m_ContextType;

	TranslationUnit* m_pTransUnit;

	//The current start state of flex.
	int m_nStartState;

	void* m_pScanInfo;

	//The "buffer state" of flex.
	void* m_pBuffer;

	//defer the EOF token processing
	bool m_bDeferEof;

	//type-argument nesting depth (see GenericDepth()).
	int m_nGenericDepth;

	//previous token was "List"/"Dict" (see PendingGenericOpen()).
	bool m_bPendingGenericOpen;

	//previous token was "class" (see MarkClassDeclHead()).
	bool m_bPendingClassDeclName;

	//0.13.1a Task 3: the '<' after an identifier probing as a generic
	//receiver opener (see ProbeTypeArgumentOpener). Consumed by the
	//'< rule like the builtin flag.
	bool m_bPendingReceiverLt;

	//0.13.1c identity rescue: forced comparison reading (see
	//SetForceComparisonReads). Default-initialized so OpenString's
	//InitScanInfo reset cannot clear it.
	bool m_bForceComparisonReads = false;

	//The normalized source, kept while a translation unit is open so the
	//spelling probe can look past the current token (flex exposes no
	//position API). Freed by CloseFile/CloseString.
	std::string m_sScanText;

	//Byte offset of each line start in m_sScanText; maps the lexer's
	//(line, byte column) to an absolute offset for the probe.
	std::vector<size_t> m_scanLineStarts;
};

} //namespace nlang
