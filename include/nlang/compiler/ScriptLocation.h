/*-----------------------------------------------------------------------------
ncomp/intf/ScriptLocation.h
This file define the interface of a script location in the nlang compiler.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include <string>

namespace nlang
{

//A position in a script translation unit of nlang.
class NLANG_COMPILER_API ScriptLocation: public ISourceLocation
{
public:
	ScriptLocation();

	std::unique_ptr<ISourceLocation> Clone() const override;

	std::string ToString() const override;

	TranslationUnit* TransUnit() const override;

	size_t m_nStartLine; //start form 1
	size_t m_nEndLine;
	size_t m_nStartCol;  //start form 1
	size_t m_nEndCol;
	TranslationUnit* m_pTransUnit;
	//File-name snapshot taken by Clone(): the TU above is owned by the
	//ModuleBuilder and dies with it, while cloned log items must stay
	//readable after collection (ListCompileLogger contract, embed spec
	//D9). Empty on parser-minted locations -- the YYLLOC_DEFAULT macros
	//copy the fields above one by one and never touch this member, so
	//the parse stacks stay snapshot-free by construction.
	std::string m_sFileName;
};

} //namespace nlang

//The source location type used by flex.
typedef ::nlang::ScriptLocation YYLTYPE;

/* alert the parser that we have our own definition */
# define YYLTYPE_IS_DECLARED 1
# define YYLLOC_DEFAULT(Current, Rhs, N)									\
    do {																	\
		if (N)																\
		{																	\
			(Current).m_nStartLine    = YYRHSLOC(Rhs, 1).m_nStartLine;		\
			(Current).m_nEndLine      = YYRHSLOC(Rhs, N).m_nEndLine;		\
			(Current).m_nStartCol     = YYRHSLOC(Rhs, 1).m_nStartCol;		\
			(Current).m_nEndCol       = YYRHSLOC(Rhs, N).m_nEndCol;			\
			(Current).m_pTransUnit	  = YYRHSLOC(Rhs, 1).m_pTransUnit;		\
		}																	\
		else																\
		{																	\
			/* empty RHS */													\
			(Current).m_nStartLine   = (Current).m_nEndLine  =				\
				YYRHSLOC(Rhs, 0).m_nEndLine;								\
			(Current).m_nStartCol = (Current).m_nEndCol =					\
				YYRHSLOC(Rhs, 0).m_nEndCol;									\
			(Current).m_pTransUnit  = YYRHSLOC(Rhs, 0).m_pTransUnit;		\
		}																	\
	} while (0)