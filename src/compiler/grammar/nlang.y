/*bison version 3.8.2 */
%require "3.0"
%define api.pure
%parse-param { nlang::ScriptParser &parser }
%parse-param { yyscan_t yyscanner }
%lex-param { yyscan_t yyscanner }
%locations
%debug

%code requires {

#include "ScriptLocation.h"
#include "SyntaxTree.h"

typedef void *yyscan_t;

namespace nlang
{

class ScriptParser;

} //namespace nlang

} /*%code requires */

%{

/*Text code parser for N-Language parser by yacc/bison */
#include "ScriptParser.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <string>
#include <vector>
#include "SnExpressions.h"
#include "SyntaxTree.h"
#include <nlang/runtime/RnData.h>
using namespace nlang;

//Phase 9b: scan string literal content for ${identifier} interpolation.
//Returns SnLiteralExpr if no ${...} found; otherwise builds OP_Add tree.
//$$ escape: $$ -> $ (literal dollar).
static SnExpression* BuildStringExpr(
	std::string* pRawText, ISourceLocation& loc, ScriptParser& parser)
{
	//Fast path: no '$' at all -> plain literal. Transfer pRawText ownership.
	if (pRawText->find('$') == std::string::npos)
		return new SnLiteralExpr(*RnString::Instance(), pRawText, loc);

	//Interpolation path: copy content then free lexer's buffer.
	std::string s = *pRawText;
	delete pRawText;
	const size_t n = s.size();

	//NLang identifier rule: [A-Za-z_][A-Za-z0-9_]*  (matches lexer Name rule).
	auto isValidIdent = [](const std::string& nm) {
		if (nm.empty()) return false;
		char c0 = nm[0];
		if (!(std::isalpha(static_cast<unsigned char>(c0))
		      || c0 == '_')) return false;
		for (size_t k = 1; k < nm.size(); ++k) {
			char c = nm[k];
			if (!(std::isalnum(static_cast<unsigned char>(c))
			      || c == '_')) return false;
		}
		return true;
	};

	std::vector<SnExpression*> parts;
	std::string lit;
	size_t i = 0;
	while (i < n)
	{
		if (s[i] == '$' && i + 1 < n)
		{
			if (s[i + 1] == '$') { lit.push_back('$'); i += 2; continue; }
			if (s[i + 1] == '{')
			{
				//Flush accumulated literal (if non-empty).
				if (!lit.empty()) {
					parts.push_back(new SnLiteralExpr(
						*RnString::Instance(), new std::string(lit), loc));
					lit.clear();
				}
				//Find closing '}'
				size_t j = i + 2;
				while (j < n && s[j] != '}') ++j;
				if (j >= n) {
					parser.Log(CLL_Error, loc,
						"unterminated interpolation: missing '}' in \"${...}\".");
					//Free any parts already allocated before bailing.
					for (auto* p : parts) delete p;
					return new SnLiteralExpr(*RnString::Instance(),
						new std::string(""), loc);
				}
				//Extract identifier text between ${ and }
				std::string idName = s.substr(i + 2, j - i - 2);
				if (idName.empty()) {
					parser.Log(CLL_Error, loc,
						"empty interpolation ${}: identifier required.");
				} else if (!isValidIdent(idName)) {
					parser.Log(CLL_Error, loc,
						"invalid identifier \"%s\" in ${...}: "
						"only ${name} supported (no expressions).",
						idName.c_str());
				} else {
					//Undefined identifiers are not checked here; resolver
					//reports "undefined identifier" later, preserving the
					//expected error path.
					parts.push_back(new SnIdentifierExpr(
						new std::string(idName), loc));
				}
				i = j + 1;
				continue;
			}
		}
		lit.push_back(s[i]);
		++i;
	}
	//Flush trailing literal.
	if (!lit.empty()) {
		parts.push_back(new SnLiteralExpr(
			*RnString::Instance(), new std::string(lit), loc));
	}

	//No interpolation parts (e.g., only invalid ${} that logged errors).
	if (parts.empty())
		return new SnLiteralExpr(*RnString::Instance(),
			new std::string(""), loc);

	//If first part is an identifier (not string literal), prepend empty
	//string literal "" to force string context. Otherwise two int
	//identifiers would produce arithmetic OP_Add_i32 instead of concat.
	//   "${a}${b}" with a,b both int -> Add(Add("", a), b) -> string ctx.
	if (parts[0]->Kind() != NK_LiteralExpr) {
		auto* pEmpty = new SnLiteralExpr(*RnString::Instance(),
			new std::string(""), loc);
		parts.insert(parts.begin(), pEmpty);
	}

	//Left-associative fold: ((p0 + p1) + p2) + ...
	SnExpression* result = parts[0];
	for (size_t k = 1; k < parts.size(); ++k) {
		result = new SnBinaryExpr(SnBinaryExpr::OP_Add, result, parts[k], loc);
	}
	return result;
}

%}

/*data value union */
%union {
    nlang::nchar           					v_Char;
    nlang::int8         					v_Byte;
    nlang::uint8        					v_UByte;
    nlang::int16        					v_Short;
    nlang::uint16       					v_UShort;
    nlang::int32        					v_Int;
    nlang::uint32       					v_UInt;
    float               					v_Float;
    std::string *           				v_pStr;
    nlang::SnUsing *						v_pUsing;
    nlang::PtrList<nlang::SnUsing> *		v_pUsingList;
    std::vector<std::string> *             v_pImportList;
	nlang::SnNamespace *					v_pNamespace;
    nlang::SnFunction  *      				v_pFunction;
	nlang::PtrList<nlang::SnFormalParam> *	v_pFormalParamList;
	nlang::SnFormalParam *					v_pFormalParam;
    nlang::SnStatement *					v_pStatement;
    nlang::SnParagraph  *					v_pParagraph;
    nlang::PtrList<nlang::SnStatement> *	v_pStatementList;
	nlang::PtrList<nlang::SnExpression> *	v_pExpressionList;
    nlang::SnExpression *					v_pExpression;
	nlang::SnMemberExpr *					v_pMemberExpr;
	nlang::SnInvokeExpr *					v_pInvokeExpr;
	nlang::SnIdentifierExpr *				v_pIdentifierExpr;
	nlang::SnNameExpr *						v_pNameExpr;
	nlang::SnFieldExpr *					v_pFieldExpr;
	nlang::SnBinaryExpr *					v_pBinaryExpr;
	nlang::SnLocalDeclStmt *				v_pLocalDeclStmt;
	nlang::SnAssignStmt *					v_pAssignStmt;
	nlang::SnCompoundAssignStmt *			v_pCompoundAssignStmt;
	nlang::SnSubscriptAssignStmt *			v_pSubscriptAssignStmt;
	nlang::SnSubscriptExpr *				v_pSubscriptExpr;
	nlang::SnNewArrayExpr *					v_pNewArrayExpr;
	nlang::SnIfStmt *						v_pIfStmt;
	nlang::SnWhileStmt *					v_pWhileStmt;
	nlang::SnDoStmt *						v_pDoStmt;
	nlang::SnForStmt *						v_pForStmt;
	nlang::SnForeachStmt *					v_pForeachStmt;
	nlang::SnSwitchStmt *					v_pSwitchStmt;
	nlang::SnCaseClause *					v_pCaseClause;
	std::vector<nlang::SnCaseClause*> *		v_pCaseClauseList;
	nlang::SnTryStmt *						v_pTryStmt;
	nlang::SnCatchClause *					v_pCatchClause;
	std::vector<nlang::SnCatchClause*> *	v_pCatchClauseList;
	nlang::SnThrowStmt *					v_pThrowStmt;
	nlang::SnBreakStmt *					v_pBreakStmt;
	nlang::SnContinueStmt *				v_pContinueStmt;
	nlang::SnEnumDecl *					v_pEnumDecl;
	nlang::SnEnumMember *				v_pEnumMember;
	nlang::PtrList<nlang::SnEnumMember> *	v_pEnumMemberList;
	nlang::PtrList<nlang::SnFunction> *	v_pEnumMethodList;
	nlang::SnStructDecl *				v_pStructDecl;
	nlang::SnStructField *				v_pStructField;
	nlang::PtrList<nlang::SnStructField> *	v_pStructFieldList;
	nlang::SnClassDecl *				v_pClassDecl;
	nlang::PtrList<nlang::SnField> *	v_pClassMemberList;
	nlang::SnInterfaceDecl *			v_pInterfaceDecl;
	nlang::PtrList<nlang::SnFieldExpr> *	v_pNameExprList;
	std::vector<nlang::SnFieldExpr*>*	v_pFieldExprVec;
	std::vector<nlang::SnLocalDeclStmt::LocalDecl> * v_pLocalDeclList;
    nlang::PtrList<nlang::SnField> *		v_pMemberList;
	nlang::SnField *						v_pField;
	nlang::NodeBits							v_NodeFlags;
	nlang::FieldAccessType					v_AccessType;
	std::vector<nlang::InitEntry>*			v_pInitEntryList;
	nlang::InitEntry*						v_pInitEntry;
}

%destructor { } <v_Char> <v_Byte> <v_UByte> <v_Short> <v_UShort> <v_Int> <v_UInt> <v_Float>
%destructor { } <v_NodeFlags> <v_AccessType>
%destructor { delete $$; } <*>

%printer { fprintf (yyoutput, "%d", $$); } <v_Byte> <v_Short> <v_Int>
%printer { fprintf (yyoutput, "%g", $$); } <v_Float>
%printer { fprintf (yyoutput, "%u", $$); } <v_UByte> <v_UShort> <v_UInt> <v_NodeFlags> <v_AccessType>
%printer { fprintf (yyoutput, "\"%s\"", $$->c_str()); } <v_pStr>
%printer { fprintf (yyoutput, "&%p", (void*)$$); } <*>

/*declare nonterminals */
%type <v_AccessType>    		AccessType
%type <v_NodeFlags>    			NodeFlags NodeFlag
%type <v_pNameExpr>				NameExpr
%type <v_pFieldExpr>				Type TypeArg
%type <v_pFieldExprVec>			TypeList
%type <v_pIdentifierExpr>		IdentifierExpr
%type <v_pInvokeExpr>			InvokeExpr
%type <v_pMemberExpr>			MemberExpr
%type <v_pUsing>				Using
%type <v_pUsingList>			UsingList
%type <v_pImportList>			ImportList
%type <v_pNamespace>			Namespace
%type <v_pField>				NamespaceMember
%type <v_pMemberList>			NamespaceMemberList
%type <v_pFormalParam>			FormalParam
%type <v_pFormalParamList>		FormalParamList
%type <v_pExpression>			Expression ParenthesesExpr LiteralExpr NewExpr NewArrayExpr SubscriptExpr
%type <v_pExpressionList>		ConcreteParamList
%type <v_pExpression>			ConcreteParam
%type <v_pInitEntryList>		InitListElements InitListElementList InitEntries InitEntryList
%type <v_pInitEntry>			InitEntry
%type <v_pFunction>				Function FunctionHeader
%type <v_pParagraph>			Paragraph FunctionBody DefaultCase FunctionBodyOrSemi
%type <v_pStatementList>		StatementList
%type <v_pStatement>			Statement ReturnStmt InvokeStmt LocalDeclStmt AssignStmt CompoundAssignStmt AssertStmt SubscriptAssignStmt IfStmt WhileStmt InitFor FiniFor TryStmt ThrowStmt SuperCallStmt FinallyClauseOpt
%type <v_pForStmt>		ForStmt
%type <v_pForeachStmt>	ForeachStmt
%type <v_pCatchClause>		CatchClause
%type <v_pCatchClauseList>	CatchClauseList
%type <v_pDoStmt>		DoStmt
%type <v_pSwitchStmt>	SwitchStmt
%type <v_pExpressionList>	CaseLabelList
%type <v_pCaseClause>	CaseClause
%type <v_pCaseClauseList>	CaseClauseList
%type <v_pBreakStmt>		BreakStmt
%type <v_pContinueStmt>	ContinueStmt
%type <v_pLocalDeclList>		LocalDeclList
%type <v_pEnumDecl>			EnumDecl
%type <v_pEnumMember>		EnumMember
%type <v_pEnumMemberList>	EnumMemberList
%type <v_pEnumMethodList>	EnumMethodSection EnumMethodList
%type <v_pFunction>		EnumMethod
%type <v_pStructDecl>		StructDecl
%type <v_pStructField>		StructField
%type <v_pStructFieldList>	StructFieldList
%type <v_pClassDecl>		ClassDecl
%type <v_pInterfaceDecl>	InterfaceDecl
%type <v_pNameExprList>		ImplementsOpt NameList
%type <v_pClassMemberList>	InterfaceMemberList
%type <v_pField>			InterfaceMember
%type <v_pField>			ClassMember
%type <v_pClassMemberList>	ClassMemberList
%type <v_pNameExpr>			ClassInheritOpt

%start CompileUnit

/*declare tokens */
%token <v_Char>		TT_Char
%token <v_pStr>		TT_Identifier
%token <v_pStr>		TT_String /*Note: TT_String is not KT_String */
%token <v_Byte>		TT_Byte
%token <v_UByte>	TT_UByte
%token <v_Short>	TT_Short
%token <v_UShort>	TT_UShort
%token <v_Int>		TT_Int
%token <v_UInt>		TT_UInt
%token <v_Float>	TT_Float
%token TT_Comment	TT_Error

/*keyword type */
/*NOTE: keyword tokens KT_Bool..KT_While must stay one contiguous
  block -- nide's SyntaxHighlighter colors the whole [KT_Bool, KT_While]
  number range as keywords. */
%token KT_Bool
%token KT_As
%token KT_Assert
%token KT_Break
%token KT_Byte
%token KT_Case
%token KT_Catch
%token KT_Char
%token KT_Class
%token KT_Const
%token KT_Continue
%token KT_Do
%token KT_Default
%token KT_Else
%token KT_Elseif
%token KT_Enum
%token KT_False
%token KT_Finally
%token KT_Float
%token KT_For
%token KT_Foreach
%token KT_If
%token KT_Implements
%token KT_Import
%token KT_In
%token KT_Int
%token KT_Interface
%token KT_Namespace
%token KT_Native
%token KT_New
%token KT_Null
%token KT_Out
%token KT_Private
%token KT_Protected
%token KT_Public
%token KT_Return
%token KT_Short
%token KT_State
%token KT_Static
%token KT_String
%token KT_Struct
%token KT_Super
%token KT_Switch
%token KT_This
%token KT_Throw
%token KT_True
%token KT_Try
%token KT_Ubyte
%token KT_Uint
%token KT_Ushort
%token KT_Using
%token KT_Virtual
%token KT_Void
%token KT_While

/*double-character operator type */
%token OT_INC
%token OT_DEC
%token OT_LSH
%token OT_RSH
%token OT_LE
%token OT_GE
%token OT_NE
%token OT_EQ
%token OT_AEQ
%token OT_AND
%token OT_OR
%token OT_MODS
%token OT_BANDS
%token OT_MULS
%token OT_INCS
%token OT_DECS
%token OT_DIVS
%token OT_BORS
%token OT_Brackets

%nonassoc P_Then
%nonassoc KT_Else

%right OT_BORS OT_DIVS OT_DECS OT_INCS OT_MULS OT_BANDS OT_MODS
%left OT_OR
%left OT_AND
%left '|'
%left '^'
%left '&'
%left OT_AEQ OT_EQ OT_NE
%left OT_GE '>' OT_LE '<'
%left OT_RSH OT_LSH
%left '-' '+'
%left '/' '*' '%'
%right '~' OT_DEC P_Minus OT_INC '!'
%left KT_As

%nonassoc P_NonMember
%left '.' '['
%nonassoc P_Field
%nonassoc '(' ')'
%nonassoc P_ClassMethod
%nonassoc P_ClassField

%code provides {

#include "TranslationUnit.h"
#include "BuildEnvironment.h"
#include "Utils.h"

#ifndef YY_USER_DEFS_
#define YY_USER_DEFS_

namespace nlang
{

typedef yyscan_t NScanInfo;
typedef YYSTYPE NToken;

} //namespace nlang

void yyerror(YYLTYPE *loc, nlang::ScriptParser&, yyscan_t, char *s, ...);

extern int yylex \
    (YYSTYPE *yylval_param, YYLTYPE *yylloc_param, yyscan_t yyscanner);


#endif //YY_USER_DEFS_

}

%%

CompileUnit:	ImportList UsingList NamespaceMemberList {
						TranslationUnit *pTransUnit = parser.TransUnit();
						pTransUnit->Init($2, $3, @3);
						pTransUnit->SetImports($1);
					} ;

ImportList:	ImportList KT_Import TT_String ';' {
						$1->push_back(*($3));
						delete $3;
						$$ = $1;
					} |
					{
						/*on empty */
						$$ = new std::vector<std::string>();
					} ;

UsingList:	UsingList Using {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = new PtrList<SnUsing>();
				} ;

Using:	KT_Using NameExpr ';' {
			   $$ = new SnUsing($2, @2);
			} |
			//Phase 13: type alias form. The NameExpr is the alias name; the
			//Type is the aliased target, expanded at use sites by the TU-level
			//alias pre-pass (runs before translation units are merged).
			KT_Using NameExpr '=' Type ';' {
			   $$ = new SnUsing($2, $4, @2);
			} ;

NamespaceMemberList:	NamespaceMemberList NamespaceMember {
								$1->push_back($2);
								$$ = $1;
							} |
							{
								/*on empty */
								$$ = new PtrList<SnField>();
							} ;

NamespaceMember:	Namespace {
							$$ = $1;
						} |
						Function {
							$$ = $1;
						} |
						EnumDecl {
							$$ = $1;
						} |
						StructDecl {
							$$ = $1;
						} |
						ClassDecl {
							$$ = $1;
						} |
						InterfaceDecl {
							$$ = $1;
						} ;

Namespace:	KT_Namespace TT_Identifier '{' NamespaceMemberList '}' {
					$$ = new SnNamespace($2, $4, @1);
				} ;

Function:	FunctionHeader FunctionBody {
					$1->Body($2);
					$$ = $1;
				} |
				FunctionHeader ';' {
					$1->AddFlags(NF_Abstract);
					$$ = $1;
				} ;

FunctionHeader:	AccessType NodeFlags Type TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2, $3, $4, $6, @2);
					}
					| AccessType NodeFlags KT_Void TT_Identifier '(' FormalParamList ')' {
						$$ = new SnFunction($1, $2, nullptr, $4, $6, @2);
					} ;

FunctionBody:	Paragraph {
						$$ = $1;
					} ;

FormalParamList:	FormalParamList ',' FormalParam {
							if ($1->empty())
								parser.Log(CLL_Error, @1, "Unexpected ',' in a param list.");
							else
								$1->push_back($3);
							$$ = $1;
						} |
						FormalParam {
							$$ = new PtrList<SnFormalParam>();
							$$->push_back($1);
						} |
						{
							/*on empty */
							$$ = new PtrList<SnFormalParam>();
						} ;

FormalParam:	NodeFlags Type TT_Identifier '=' Expression {
						$$ = new SnFormalParam($1, $2, $3, $5, @1);
					} |
					NodeFlags Type TT_Identifier {
						$$ = new SnFormalParam($1, $2, $3, nullptr, @1);
					} |
					/* Phase 9e: out parameter. No default-value form — an out
					 * parameter is callee-assigned, a default is meaningless. */
					NodeFlags KT_Out Type TT_Identifier {
						$$ = new SnFormalParam($1, $3, $4, nullptr, @2);
						$$->AddFlags(NF_Out);
					} ;

Paragraph:	'{' StatementList '}' {
					$$ = new SnParagraph($2, @1);
				} ;

StatementList:	StatementList Statement {
						if ($2 != nullptr)
							$1->push_back($2);
						$$ = $1;
					} |
					{
						//on empty
						$$ = new PtrList<SnStatement>();
					} ;

Statement:	';' {
					parser.Log(CLL_Warn, @1, "Empty statement will be ignored.");
					$$ = nullptr;
				} |
				ReturnStmt {
					$$ = $1;
				} |
				InvokeStmt {
					$$ = $1;
				} |
				LocalDeclStmt {
					$$ = $1;
				} |
				AssignStmt {
					$$ = $1;
				} |
				CompoundAssignStmt {
					$$ = $1;
				} |
				AssertStmt {
					$$ = $1;
				} |
				SubscriptAssignStmt {
					$$ = $1;
				} |
				IfStmt {
					$$ = $1;
				} |
				WhileStmt {
					$$ = $1;
				} |
				DoStmt {
					$$ = $1;
				} |
				ForStmt {
					$$ = $1;
				} |
				ForeachStmt {
					$$ = $1;
				} |
				SwitchStmt {
					$$ = $1;
				} |
				BreakStmt {
					$$ = $1;
				} |
				ContinueStmt {
					$$ = $1;
				} |
				TryStmt {
					$$ = $1;
				} |
				ThrowStmt {
					$$ = $1;
				} |
				SuperCallStmt {
					$$ = $1;
				} |
				Paragraph {
					$$ = $1;
				} |
				error ';'
				{
					parser.Log(CLL_Error, @1, "Invalid statement.");
					$$ = nullptr;
				} ;

ReturnStmt: KT_Return Expression ';' {
					$$ = new SnReturnStmt($2, @1);
				} |
				//Void support: bare `return;` for early exit from void functions.
				KT_Return ';' {
					$$ = new SnReturnStmt(nullptr, @1);
				} ;

InvokeStmt: InvokeExpr ';' { $$ = new SnInvokeStmt($1, @1); } |
				MemberExpr ';' { $$ = new SnInvokeStmt($1, @1); } ;

/*
Local variable declaration statement.
Reference: EN's LocalDeclStmt (compiler_bak/grammer/nlang.y:535).
Phase 9a: `const Type decls;` form marks all locals as const (NF_Const).
*/
LocalDeclStmt: Type LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($1, $2, @1);
					} |
					KT_Const Type LocalDeclList ';' {
						$$ = new SnLocalDeclStmt($2, $3, true, @1);
					} ;

LocalDeclList: LocalDeclList ',' TT_Identifier {
						$1->push_back({*$3, nullptr});
						$$ = $1;
					} |
					LocalDeclList ',' TT_Identifier '=' Expression {
						$1->push_back({*$3, $5});
						$$ = $1;
					} |
					TT_Identifier '=' Expression {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, $3});
					} |
					TT_Identifier {
						$$ = new std::vector<SnLocalDeclStmt::LocalDecl>();
						$$->push_back({*$1, nullptr});
					} ;

/*
Assignment statement.
Reference: EN's AssignStmt (compiler_bak/grammer/nlang.y:546).
*/
AssignStmt: IdentifierExpr '=' Expression ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} |
				MemberExpr '=' Expression ';' {
					$$ = new SnAssignStmt($1, $3, @1);
				} ;

/*
Compound assignment statement (e.g. x += 1, arr[i] *= 2).
Phase 9a: left-value is evaluated only once.
*/
CompoundAssignStmt: IdentifierExpr OT_INCS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				IdentifierExpr OT_DECS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				IdentifierExpr OT_MULS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				IdentifierExpr OT_DIVS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				IdentifierExpr OT_MODS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} |
				MemberExpr OT_INCS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Add, $1, $3, @1);
				} |
				MemberExpr OT_DECS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Sub, $1, $3, @1);
				} |
				MemberExpr OT_MULS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mul, $1, $3, @1);
				} |
				MemberExpr OT_DIVS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Div, $1, $3, @1);
				} |
				MemberExpr OT_MODS Expression ';' {
					$$ = new SnCompoundAssignStmt(SnBinaryExpr::OP_Mod, $1, $3, @1);
				} ;

/*
Assert statement: assert(cond); - exit(1) on failure.
Phase 9a: simple runtime check.
*/
AssertStmt: KT_Assert '(' Expression ')' ';' {
					$$ = new SnAssertStmt($3, @1);
				} ;

/*
Phase 9d: try/catch statement. Phase 9d-2 adds the optional finally
clause (full Java semantics: finally runs on normal, catch, break/
continue/return, and exception paths).
The try body is a single Statement (typically a Paragraph block).
CatchClauseList allows zero or more catch clauses (zero is meaningful
only together with finally — `try {} finally {}`; a resolver-stage check
rejects try with neither catches nor finally).
*/
TryStmt: KT_Try Statement CatchClauseList FinallyClauseOpt {
					$$ = new SnTryStmt($2, $3, $4, @1);
				} ;

CatchClauseList: /* empty */ {
					$$ = new std::vector<SnCatchClause*>();
				} | CatchClauseList CatchClause {
					$$ = $1;
					if ($2) $$->push_back($2);
				} ;

CatchClause: KT_Catch '(' Type TT_Identifier ')' Statement {
					$$ = new SnCatchClause($3, *$4, $6, @1);
				} ;

FinallyClauseOpt: /* empty */ {
					$$ = nullptr;
				} | KT_Finally Statement {
					$$ = $2;
				} ;

/*
Phase 9d: throw statement. `throw <expr>;` raises the given Exception
instance; `throw;` (no operand) re-raises the in-flight exception of the
enclosing catch handler.
*/
ThrowStmt: KT_Throw Expression ';' {
					$$ = new SnThrowStmt($2, @1);
				} | KT_Throw ';' {
					$$ = new SnThrowStmt(nullptr, @1);
				} ;

/*
Phase 9d-2: super(...) call statement — forwards constructor arguments
to the direct parent class constructor. Legal anywhere inside a user
constructor (not required to be the first statement). Reuses
ConcreteParamList (positional args; named args are rejected by the
resolver).
*/
SuperCallStmt: KT_Super '(' ConcreteParamList ')' ';' {
					$$ = new SnSuperCallStmt(*$3, @1);
				} ;

/*
Array subscript assignment statement (e.g. arr[i] = value).
*/
SubscriptAssignStmt: Expression '[' Expression ']' '=' Expression ';' {
					$$ = new SnSubscriptAssignStmt($1, $3, $6, @1);
				} ;

/*
If/else statement.
Reference: EN's IfStmt (compiler_bak/grammer/nlang.y:554).
*/
IfStmt:	KT_If '(' Expression ')' Statement %prec P_Then {
				$$ = new SnIfStmt($3, $5, nullptr, @1);
			} |
			KT_If '(' Expression ')' Statement KT_Else Statement {
				$$ = new SnIfStmt($3, $5, $7, @1);
			} ;


/*
While loop statement.
Reference: EN's WhileStmt (compiler_bak/grammer/nlang.y:561).
*/
WhileStmt:	KT_While '(' Expression ')' Statement {
					$$ = new SnWhileStmt($3, $5, @1);
				} ;

/*
Do-while loop statement.
Reference: EN's DoStmt (compiler_bak/grammer/nlang.y:565).
*/
DoStmt:	KT_Do Statement KT_While '(' Expression ')' ';' {
				$$ = new SnDoStmt($5, $2, @1);
			} ;

/*
For loop statement.
Reference: EN's ForStmt (compiler_bak/grammer/nlang.y:569).
*/
ForStmt:	KT_For '(' InitFor ';' Expression ';' FiniFor ')' Statement {
					$$ = new SnForStmt($3, $5, $7, $9, @1);
				} ;

InitFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				Type LocalDeclList {
					$$ = new SnLocalDeclStmt($1, $2, @1);
				} |
				InvokeExpr {
					$$ = new SnInvokeStmt($1, @1);
				} |
				IdentifierExpr '=' Expression {
					$$ = new SnAssignStmt($1, $3, @1);
				} ;

FiniFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				InvokeExpr {
					$$ = new SnInvokeStmt($1, @1);
				} |
				IdentifierExpr '=' Expression {
					$$ = new SnAssignStmt($1, $3, @1);
				} ;

/*
Foreach loop statement (Phase 8e-5).
Iterates Array / List<T> / Dict<K,V> (keys).
*/
ForeachStmt:	KT_Foreach '(' Type TT_Identifier KT_In Expression ')' Statement {
					$$ = new SnForeachStmt($3, *$4, $6, $8, @1);
				} ;

/*
Break statement.
Reference: EN's BreakStmt (compiler_bak/grammer/nlang.y:624).
*/
BreakStmt:	KT_Break ';' {
					$$ = new SnBreakStmt(@1);
				} ;

/*
Continue statement.
Reference: EN's ContinueStmt (compiler_bak/grammer/nlang.y:628).
*/
ContinueStmt:	KT_Continue ';' {
					$$ = new SnContinueStmt(@1);
				} ;

/*
Switch statement.
Reference: EN's SwitchStmt (compiler_bak/grammer/nlang.y:598).
*/
SwitchStmt:	KT_Switch '(' Expression ')' '{' CaseClauseList DefaultCase '}' {
				$$ = new SnSwitchStmt($3, $6, $7, @1);
			} ;

CaseClauseList:	CaseClauseList CaseClause {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = new std::vector<SnCaseClause*>();
				} ;

//Phase 12: comma-separated case labels — `case 1, 2:` enters the clause
//body when ANY label matches. PtrList<SnExpression> follows ConcreteParamList.
CaseLabelList:	CaseLabelList ',' Expression {
					$1->push_back($3);
					$$ = $1;
			} |
				Expression {
					$$ = new PtrList<SnExpression>();
					$$->push_back($1);
			} ;

CaseClause:	KT_Case CaseLabelList ':' StatementList {
				$$ = new SnCaseClause($2, $4, @1);
			} ;

DefaultCase:	{
					/*on empty */
					$$ = nullptr;
				} |
				KT_Default ':' StatementList {
					$$ = new SnParagraph($3, @1);
				} ;
/*
Enum type declaration.
*/
EnumDecl:	KT_Enum TT_Identifier '{' EnumMemberList EnumMethodSection '}' {
					$$ = new SnEnumDecl($2, $4, $5, @1);
				} ;

EnumMemberList:	EnumMemberList ',' EnumMember {
					$1->push_back($3);
					$$ = $1;
				} |
				EnumMember {
					$$ = new PtrList<SnEnumMember>();
					$$->push_back($1);
				} ;

EnumMember:	TT_Identifier {
				$$ = new SnEnumMember($1, nullptr, @1);
				} |
				TT_Identifier '=' Expression {
					$$ = new SnEnumMember($1, $3, @1);
				} ;

/*
Optional method table, separated from the member list by a ';' tail.
The lone ';' production accepts the Java-style "enum E { A; }" closing
separator with no methods following.
*/
EnumMethodSection:	';' EnumMethodList {
				$$ = $2;
			} |
				';' {
				$$ = new PtrList<SnFunction>();
			} |
				/* empty */ {
				$$ = new PtrList<SnFunction>();
			} ;

EnumMethodList:	EnumMethodList EnumMethod {
				$1->push_back($2);
				$$ = $1;
			} |
				EnumMethod {
				$$ = new PtrList<SnFunction>();
				$$->push_back($1);
			} ;

/*
Enum methods take no NodeFlag modifiers (no static/override/native),
unlike ClassMember. Bodyless declarations still parse (FunctionBodyOrSemi)
so the resolver can reject them with a proper diagnostic.
*/
EnumMethod:	AccessType Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
				auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
				if ($7 == nullptr)
					func->AddFlags(NF_Abstract);
				else
					func->Body($7);
				$$ = func;
			} |
				AccessType KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
				auto* func = new SnFunction($1, NF_NONE, nullptr, $3, $5, @2);
				if ($7 == nullptr)
					func->AddFlags(NF_Abstract);
				else
					func->Body($7);
				$$ = func;
			} ;

/*
Struct type declaration.
*/
StructDecl:	KT_Struct TT_Identifier '{' StructFieldList '}' {
					$$ = new SnStructDecl($2, $4, @1);
				} ;

StructFieldList:	StructFieldList StructField {
					$1->push_back($2);
					$$ = $1;
				} |
					StructField {
					$$ = new PtrList<SnStructField>();
					$$->push_back($1);
				} ;

StructField:	Type TT_Identifier ';' {
					$$ = new SnStructField($1, $2, @1);
				} ;


/*
Class type declaration.
*/
ClassDecl:	KT_Class TT_Identifier ClassInheritOpt ImplementsOpt '{' ClassMemberList '}' {
					auto* pClass = new SnClassDecl($2, $3, $6, @1);
					if ($4)
						for (auto *pNode : *$4)
							pClass->AddImplementsName(pNode);
					$$ = pClass;
				} ;

ClassInheritOpt:	':' NameExpr { $$ = $2; } |
					{ $$ = nullptr; } ;

/*
Optional "implements I1, I2" clause on a class. Empty when omitted.
The list carries SnFieldExpr* (name expressions) resolved to interface
decls during semantic analysis.
*/
ImplementsOpt:	KT_Implements NameList {
					$$ = $2;
				} |
				{
					$$ = nullptr;
				} ;

/*
Comma-separated list of NameExpr values (for the implements clause).
Returns a PtrList<SnFieldExpr> owning the name expressions.
*/
NameList:	NameExpr {
					$$ = new PtrList<SnFieldExpr>();
					$$->push_back($1);
				} |
				NameList ',' NameExpr {
					$1->push_back($3);
					$$ = $1;
				} ;

/*
Interface type declaration. Members are method signatures only (no
fields, no bodies). An interface establishes a contract that
implementing classes must satisfy.
*/
InterfaceDecl:	KT_Interface TT_Identifier '{' InterfaceMemberList '}' {
					$$ = new SnInterfaceDecl($2, $4, @1);
				} |
				KT_Interface TT_Identifier '{' '}' {
					$$ = new SnInterfaceDecl($2, new PtrList<SnField>(), @1);
				} ;

InterfaceMemberList:	InterfaceMemberList InterfaceMember {
					$1->push_back($2);
					$$ = $1;
				} |
				InterfaceMember {
					$$ = new PtrList<SnField>();
					$$->push_back($1);
				} ;

InterfaceMember:	AccessType NodeFlag Type TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @2);
					func->AddFlags(NF_Abstract);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
					func->AddFlags(NF_Abstract);
					$$ = func;
				}
				| AccessType NodeFlag KT_Void TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = new SnFunction($1, $2, nullptr, $4, $6, @2);
					func->AddFlags(NF_Abstract);
					$$ = func;
				}
				| AccessType KT_Void TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = new SnFunction($1, NF_NONE, nullptr, $3, $5, @2);
					func->AddFlags(NF_Abstract);
					$$ = func;
				} ;

ClassMemberList:	ClassMemberList ClassMember {
					$1->push_back($2);
					$$ = $1;
				} |
					ClassMember {
					$$ = new PtrList<SnField>();
					$$->push_back($1);
				} ;

ClassMember:	AccessType NodeFlag Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				} |
				AccessType NodeFlags Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, $3, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, $2, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |

				AccessType NodeFlag KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, nullptr, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType NodeFlags KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, $2, nullptr, $4, $6, @2);
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				}
				| AccessType KT_Void TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = new SnFunction($1, NF_NONE, nullptr, $3, $5, @2);
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |

				AccessType Type TT_Identifier ';' {
					$$ = new SnClassField($2, $3, $1, @1);
				} ;

FunctionBodyOrSemi:	Paragraph { $$ = $1; } |
				';' { $$ = nullptr; } ;

NodeFlags:	NodeFlags NodeFlag {
					const NodeBits toAdd = $2;
					if (($1  &toAdd) != 0)
					{
						parser.Log(CLL_Error, @2, "Duplicate modifier \"%s\".",
							NodeFlagInfo::NameOf(toAdd));
						$$ = $1;
					}
					else
						$$ = $1 | toAdd;
				} |
				{
					//on empty
					$$ = NF_NONE;
				} %prec P_ClassMethod ;

NodeFlag:	KT_Const 		{ $$ = NF_Const;    	} |
				KT_Static       { $$ = NF_Static;       } |
				KT_Virtual      { $$ = NF_Virtual;      } |
				KT_Native       { $$ = NF_Native;       } ;

AccessType:	KT_Private  	{ $$ = FA_Private;      } |
				KT_Protected    { $$ = FA_Protected;    } |
				KT_Public       { $$ = FA_Public;       } |
								{ $$ = FA_Default;	/*on empty */	} ;

//NameExpr is identifier-only by design. It formerly also derived
//MemberExpr (for `A.B` qualified types) — zero usage in the language,
//and the dual parentage (NameExpr|Expression both deriving MemberExpr)
//was the dominant source of reduce/reduce conflicts. Removing it (plus
//the InterfaceDecl empty-body production) took the grammar from 75 rr
//conflicts down to 1 (bison-measured). Removed in the Phase 10 audit;
//do not re-add without a real use.
//The one remaining rr conflict is on '<': `Type: NameExpr '<' TypeList '>'`
//(generic type) vs a less-than comparison. bison's reduce-first
//default picks the NameExpr/Type derivation, which keeps
//`Foo<int> x;` parsing as a declaration — the intended behavior.
NameExpr:	IdentifierExpr	{ $$ = new SnNameExpr($1, @1); } ;

//Type non-terminal used in type contexts (declarations, params, fields).
//Uses OT_Brackets ('[]' as single token) to disambiguate array type
//suffix from subscript expression (arr[i]).
//The array suffix produces a dedicated SnArrayTypeExpr node, keeping
//SnNameExpr focused on plain name expressions.
//Phase 8e-3: NameExpr '<' TypeList '>' produces SnGenericTypeExpr for
//built-in generic types like List<T>. No LALR(1) conflict because Type
//is only reached in declaration contexts where Expression is not a valid
//reduction (Type is never an Expression in NLang grammar).
Type:	NameExpr		{ $$ = $1; } |
				NameExpr '<' TypeList '>'	{ $$ = new SnGenericTypeExpr($1, $3, @1); } |
				//Phase 13: void in the first type-arg slot (Func's return
				//slot). KT_Void never derives Type, so the void spellings
				//need their own sister productions; the void node must be a
				//real type argument so `Func<void,int>` and `Func<int>`
				//produce different GenericInstKeys.
				NameExpr '<' KT_Void '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$$ = new SnGenericTypeExpr($1,
							new std::vector<nlang::SnFieldExpr*>{ pVoid }, @1);
					} |
				NameExpr '<' KT_Void ',' TypeList '>'	{
						auto* pVoid = new SnIdentifierExpr(NK_Void, @3);
						$5->insert($5->begin(), pVoid);
						$$ = new SnGenericTypeExpr($1, $5, @1);
					} |
				Type OT_Brackets	{ $$ = new SnArrayTypeExpr($1, @2); } ;

//A single type argument inside `<...>`: a plain type, or an
//out-marked type (only legal as a Func parameter slot, checked at
//instantiation). The out marker lives as an NF_Out node flag on
//the type node so it survives into GenericInstKey comparisons.
TypeArg:	Type {
						$$ = $1;
					} |
					KT_Out Type {
						$2->AddFlags(NF_Out);
						$$ = $2;
					} ;

//Comma-separated list of type arguments inside `<...>`. Used by
//the generic Type rule above and the generic NewExpr variants.
TypeList:	TypeArg {
						$$ = new std::vector<nlang::SnFieldExpr*>{ $1 };
					} |
					TypeList ',' TypeArg {
						$1->push_back($3);
						$$ = $1;
					} ;

Expression:	ParenthesesExpr	{ $$ = $1; } |
				MemberExpr		{ $$ = $1; } |
				LiteralExpr		{ $$ = $1; } |
				InvokeExpr		{ $$ = $1; } |
				IdentifierExpr	{ $$ = $1; } |
				NewExpr		{ $$ = $1; } |
				NewArrayExpr		{ $$ = $1; } |
				SubscriptExpr		{ $$ = $1; } |
				KT_This		{ $$ = new SnThisExpr(@1); } |
				KT_Null		{ auto* _nl = new SnLiteralExpr(*RnInt32::Instance(), 0, @1); _nl->AddFlags(NF_NullLiteral); $$ = _nl; } |
				//Phase 8e-6: bare `[...]` array/list literal. Only the bracket
				//form is allowed bare; dict/struct `{...}` requires explicit
				//`new Type{...}` because bare `{...}` would LALR-conflict
				//with Paragraph (block statement).
				'[' InitListElements ']'	{ $$ = new SnInitListExpr(nullptr, *$2, true, @1); delete $2; } |
				Expression '+' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Add, $1, $3, @1); } |
				Expression '-' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Sub, $1, $3, @1); } |
				Expression '*' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Mul, $1, $3, @1); } |
				Expression '/' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Div, $1, $3, @1); } |
				Expression '%' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Mod, $1, $3, @1); } |
				Expression '<' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Less, $1, $3, @1); } |
				Expression OT_LE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LessEqual, $1, $3, @1); } |
				Expression '>' Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Greater, $1, $3, @1); } |
				Expression OT_GE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_GreaterEqual, $1, $3, @1); } |
				Expression OT_EQ Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Equal, $1, $3, @1); } |
				Expression OT_NE Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_NotEqual, $1, $3, @1); } |
				Expression OT_AND Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalAnd, $1, $3, @1); } |
				Expression OT_OR Expression	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalOr, $1, $3, @1); } |
				'-' Expression %prec P_Minus	{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_Neg, $2, @1); } |
				'!' Expression					{ $$ = new SnBinaryExpr(SnBinaryExpr::OP_LogicalNot, $2, @1); } |
				Expression KT_As NameExpr		{ $$ = new SnAsExpr($1, $3, @2); } ;

ParenthesesExpr: '(' Expression ')' { $$ = $2; } ;

MemberExpr:	Expression '.' InvokeExpr		{
					$$ = new SnMemberExpr($1, $3, @1);
				} |
				Expression '.' IdentifierExpr	{
					$$ = new SnMemberExpr($1, $3, @1);
				} ;

LiteralExpr:	TT_Int		{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	$1,	@1);	} |
					TT_UInt		{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1);	} |
					TT_Short	{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1);	} |
					TT_UShort	{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1);	} |
					TT_Byte		{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1);	} |
					TT_UByte	{ $$ = new SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1);	} |
					TT_Float	{ $$ = new SnLiteralExpr(*RnFloat::Instance(),	$1,	@1);	} |
					TT_String 	{ $$ = BuildStringExpr($1, @1, parser);	} ;

InvokeExpr:	TT_Identifier '(' ConcreteParamList ')' {
					$$ = new SnInvokeExpr($1, $3, @1);
				} ;

IdentifierExpr:	TT_Identifier	{ $$ = new SnIdentifierExpr($1, @1);			} |
					KT_Int   		{ $$ = new SnIdentifierExpr(NK_Int32, @1);	} |
					KT_Float		{ $$ = new SnIdentifierExpr(NK_Float, @1);	} |
					KT_String		{ $$ = new SnIdentifierExpr(NK_String, @1);	} ;

NewExpr:	KT_New TT_Identifier '(' ConcreteParamList ')' {
					auto* pId = new SnIdentifierExpr($2, @2);
					$$ = new SnNewExpr(new SnNameExpr(pId, @2), $4, @1);
				} |
				//Phase 8e-3: generic construction `new List<int>()`.
				//Separate rule to avoid touching the plain `new Foo()` parse
				//(which has its own LALR state). $2 is the base class name;
				//$4 is the type-args list; $7 is the constructor args.
				KT_New TT_Identifier '<' TypeList '>' '(' ConcreteParamList ')' {
					auto* pId = new SnIdentifierExpr($2, @2);
					auto* pName = new SnNameExpr(pId, @2);
					$$ = new SnNewExpr(new SnGenericTypeExpr(pName, $4, @2), $7, @1);
				} |
				//Phase 8e-6: explicit collection init `new Foo{...}` /
				//`new List<int>{...}`. Initializes a fresh instance with
				//the given entries (dict key:value pairs or struct fields).
				KT_New TT_Identifier '{' InitEntries '}' {
					auto* pId = new SnIdentifierExpr($2, @2);
					auto* pName = new SnNameExpr(pId, @2);
					$$ = new SnInitListExpr(pName, *$4, false, @1);
					delete $4;
				} |
				KT_New TT_Identifier '<' TypeList '>' '{' InitEntries '}' {
					auto* pId = new SnIdentifierExpr($2, @2);
					auto* pName = new SnNameExpr(pId, @2);
					$$ = new SnInitListExpr(
						new SnGenericTypeExpr(pName, $4, @2), *$7, false, @1);
					delete $7;
				} ;

NewArrayExpr:	KT_New Type '[' Expression ']' {
					$$ = new SnNewArrayExpr($2, $4, @1);
				} |
				//Array of a generic-instantiated type: `new List<int>[2]`.
				//Separate rule for the same reason as the generic NewExpr
				//variants above: after `new Id '<' TypeList '>'` the LALR
				//stack holds the NewExpr-shaped prefix (state 58 shifts '<'
				//over the IdentifierExpr reduce), so the NameExpr-based
				//Type path never reaches the plain rule's '['.
				KT_New TT_Identifier '<' TypeList '>' '[' Expression ']' {
					auto* pId = new SnIdentifierExpr($2, @2);
					auto* pName = new SnNameExpr(pId, @2);
					$$ = new SnNewArrayExpr(
						new SnGenericTypeExpr(pName, $4, @2), $7, @1);
				} ;

SubscriptExpr:	Expression '[' Expression ']' {
					$$ = new SnSubscriptExpr($1, $3, @1);
				} ;

//Phase 8e-6: collection initializer element lists.
//InitListElements is the comma-separated value list for the array form `[...]`.
//InitEntries is the comma-separated key:value list for the brace form `{...}`.
//Each InitEntry's key form (STRING vs IDENTIFIER) decides dict vs struct at
//resolver time. Both lists may be empty (matches `[]` and `{}`).
InitListElements:	{
						$$ = new std::vector<nlang::InitEntry>();
					} |
					InitListElementList {
						$$ = $1;
					} ;

InitListElementList:	Expression {
						auto* pVec = new std::vector<nlang::InitEntry>();
						nlang::InitEntry e;
						e.keyKind = nlang::InitEntry::KeyKind::None;
						e.pValue = $1;
						pVec->push_back(std::move(e));
						$$ = pVec;
					} |
					InitListElementList ',' Expression {
						nlang::InitEntry e;
						e.keyKind = nlang::InitEntry::KeyKind::None;
						e.pValue = $3;
						$1->push_back(std::move(e));
						$$ = $1;
					} ;

InitEntries:	{
						$$ = new std::vector<nlang::InitEntry>();
					} |
					InitEntryList {
						$$ = $1;
					} ;

InitEntryList:	InitEntry {
						auto* pVec = new std::vector<nlang::InitEntry>();
						pVec->push_back(std::move(*$1));
						delete $1;
						$$ = pVec;
					} |
					InitEntryList ',' InitEntry {
						$1->push_back(std::move(*$3));
						delete $3;
						$$ = $1;
					} ;

InitEntry:	TT_String ':' Expression {
						auto* pE = new nlang::InitEntry();
						pE->keyKind = nlang::InitEntry::KeyKind::String;
						pE->keyStr = *$1;
						pE->pValue = $3;
						$$ = pE;
					} |
					TT_Identifier ':' Expression {
						auto* pE = new nlang::InitEntry();
						pE->keyKind = nlang::InitEntry::KeyKind::Identifier;
						pE->keyStr = *$1;
						pE->pValue = $3;
						$$ = pE;
					} |
					//Value-only entry for `new List<T>{v1, v2, ...}` form.
					//Resolver dispatches on target type: List → values, struct
					//→ fields in declaration order, dict → error (dict requires
					//string keys).
					Expression {
						auto* pE = new nlang::InitEntry();
						pE->keyKind = nlang::InitEntry::KeyKind::None;
						pE->pValue = $1;
						$$ = pE;
					} ;

ConcreteParamList:	ConcreteParamList ',' ConcreteParam {
							if ($1->empty())
							{
								parser.Log(CLL_Error, @2, "Invalid concrete param list, "
									"expecte param before ','.");

							}
							else
								$1->push_back($3);
							$$ = $1;
						} |
						ConcreteParam {
							$$ = new PtrList<SnExpression>();
							$$->push_back($1);
						} |
						{
							//on empty
							$$ = new PtrList<SnExpression>();
						} ;

//Phase 9c: a concrete parameter is either a positional argument (any
//expression) or a named argument `name = expr`. The `=` here is at the
//call-site parameter level — NLang has no assignment-as-expression, so
//inside `foo(...)` the form `Identifier '=' Expression` is unambiguous.
ConcreteParam:	TT_Identifier '=' Expression {
						$$ = new SnNamedArgExpr($1, $3, @1);
					} |
					/* Phase 9e: out argument. Restricted to a plain identifier
					 * at the grammar level — out targets must be assignable
					 * local slots; fields/elements are rejected here. */
					KT_Out TT_Identifier {
						auto* pId = new SnIdentifierExpr($2, @2);
						$$ = new SnOutArgExpr(pId, @1);
					} |
					Expression {
						$$ = $1;
					} ;

%%

void yyerror(YYLTYPE *pLoc, nlang::ScriptParser &parser, yyscan_t, char *szFmt, ...)
{
    va_list ap;
    va_start(ap, szFmt);
	assert(pLoc);
    parser.Env().VLog(CLL_Error, *pLoc, szFmt, ap);
    va_end(ap);
}
