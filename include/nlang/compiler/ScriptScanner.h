/*-----------------------------------------------------------------------------
	ncomp/intf/ScriptScanner.h
	This file define the interface of a lexical scanner for the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "ScriptLocation.h"

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

	//Open a script file to scan.
	bool OpenFile(const std::string& FilePath);

	//Close the file that has opened by \a OpenFile().
	void CloseFile();

	//Using a bytes string as the input stream to scan.
	void OpenString(const char* zInput);

	//Close the input string opened by \a OpenString().
	void CloseString();

	/*
	Get the next token.
	\return a value of yytokentype.
	If the return value is 0, it means that we have reached end of the input 
	stream.
	*/
	int NextToken();

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
private:
	void InitScanInfo();

	void FiniScanInfo();

	//The current token location.
	ScriptLocation m_Location;

	ContextType m_ContextType;

	TranslationUnit* m_pTransUnit;

	//The current start state of flex.
	int m_nStartState;

	void* m_pScanInfo;

	//The "buffer state" of flex.
	void* m_pBuffer;

	//defer the EOF token processing
	bool m_bDeferEof;
};

} //namespace nlang
