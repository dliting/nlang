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
using namespace nlang;

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
	nlang::SnBreakStmt *					v_pBreakStmt;
	nlang::SnContinueStmt *				v_pContinueStmt;
	nlang::SnEnumDecl *					v_pEnumDecl;
	nlang::SnEnumMember *				v_pEnumMember;
	nlang::PtrList<nlang::SnEnumMember> *	v_pEnumMemberList;
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
%destructor { EnDelete($$); } <*>

%printer { fprintf (yyoutput, "%d", $$); } <v_Byte> <v_Short> <v_Int>
%printer { fprintf (yyoutput, "%g", $$); } <v_Float>
%printer { fprintf (yyoutput, "%u", $$); } <v_UByte> <v_UShort> <v_UInt> <v_NodeFlags> <v_AccessType>
%printer { fprintf (yyoutput, "\"%s\"", $$->c_str()); } <v_pStr>
%printer { fprintf (yyoutput, "&%p", (void*)$$); } <*>

/*declare nonterminals */
%type <v_AccessType>    		AccessType
%type <v_NodeFlags>    			NodeFlags NodeFlag
%type <v_pNameExpr>				NameExpr
%type <v_pFieldExpr>				Type
%type <v_pFieldExprVec>			TypeList
%type <v_pIdentifierExpr>		IdentifierExpr
%type <v_pInvokeExpr>			InvokeExpr
%type <v_pMemberExpr>			MemberExpr
%type <v_pUsing>				Using
%type <v_pUsingList>			UsingList
%type <v_pNamespace>			Namespace
%type <v_pField>				NamespaceMember
%type <v_pMemberList>			NamespaceMemberList
%type <v_pFormalParam>			FormalParam
%type <v_pFormalParamList>		FormalParamList
%type <v_pExpression>			Expression ParenthesesExpr LiteralExpr NewExpr NewArrayExpr SubscriptExpr
%type <v_pExpressionList>		ConcreteParamList
%type <v_pInitEntryList>		InitListElements InitListElementList InitEntries InitEntryList
%type <v_pInitEntry>			InitEntry
%type <v_pFunction>				Function FunctionHeader
%type <v_pParagraph>			Paragraph FunctionBody DefaultCase FunctionBodyOrSemi
%type <v_pStatementList>		StatementList
%type <v_pStatement>			Statement ReturnStmt InvokeStmt LocalDeclStmt AssignStmt SubscriptAssignStmt IfStmt WhileStmt InitFor FiniFor
%type <v_pForStmt>		ForStmt
%type <v_pForeachStmt>	ForeachStmt
%type <v_pDoStmt>		DoStmt
%type <v_pSwitchStmt>	SwitchStmt
%type <v_pCaseClause>	CaseClause
%type <v_pCaseClauseList>	CaseClauseList
%type <v_pBreakStmt>		BreakStmt
%type <v_pContinueStmt>	ContinueStmt
%type <v_pLocalDeclList>		LocalDeclList
%type <v_pEnumDecl>			EnumDecl
%type <v_pEnumMember>		EnumMember
%type <v_pEnumMemberList>	EnumMemberList
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
%token KT_Bool
%token KT_As
%token KT_Break
%token KT_Byte
%token KT_Case
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
%token KT_Float
%token KT_For
%token KT_Foreach
%token KT_If
%token KT_Implements
%token KT_In
%token KT_Int
%token KT_Interface
%token KT_Namespace
%token KT_Native
%token KT_New
%token KT_Null
%token KT_Private
%token KT_Protected
%token KT_Public
%token KT_Return
%token KT_Short
%token KT_State
%token KT_Static
%token KT_String
%token KT_Struct
%token KT_Switch
%token KT_This
%token KT_True
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


/*token type for syntax highlighting */
enum HighlightType
{
    HTT_Default,
    HTT_Number,
    HTT_Char,
    HTT_String,
    HTT_Keyword,
    HTT_Comment,
    HTT_Error,
    HTT_COUNT
};

inline HighlightType GetHighlightType(int nTokenType)
{
    switch(nTokenType)
    {
    case TT_Byte:
    case TT_UByte:
    case TT_Short:
    case TT_UShort:
    case TT_Int:
    case TT_UInt:
    case TT_Float:
        return HTT_Number;
    case TT_Char:
        return HTT_Char;
    case TT_String:
        return HTT_String;
    case TT_Comment:
        return HTT_Comment;
    case TT_Error:
        return HTT_Error;
    default:
        if (nTokenType >= KT_Bool && nTokenType <= KT_While)
            return HTT_Keyword;
        else
            return HTT_Default;
    }
} /*%code provides */

#endif //YY_USER_DEFS_

}

%%

CompileUnit:	UsingList NamespaceMemberList {
						TranslationUnit *pTransUnit = parser.TransUnit();
						pTransUnit->Init($1, $2, @2);
					} ;

UsingList:	UsingList Using {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = EnNew(PtrList<SnUsing>());
				} ;

Using:	KT_Using NameExpr ';' {
			   $$ = EnNew(SnUsing($2, @2));
			} ;

NamespaceMemberList:	NamespaceMemberList NamespaceMember {
								$1->push_back($2);
								$$ = $1;
							} |
							{
								/*on empty */
								$$ = EnNew(PtrList<SnField>());
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
					$$ = EnNew(SnNamespace($2, $4, @1));
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
						$$ = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
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
							$$ = EnNew(PtrList<SnFormalParam>());
							$$->push_back($1);
						} |
						{
							/*on empty */
							$$ = EnNew(PtrList<SnFormalParam>());
						} ;

FormalParam:	NodeFlags Type TT_Identifier '=' Expression {
						$$ = EnNew(SnFormalParam($1, $2, $3, $5, @1));
					} |
					NodeFlags Type TT_Identifier {
						$$ = EnNew(SnFormalParam($1, $2, $3, nullptr, @1));
					} ;

Paragraph:	'{' StatementList '}' {
					$$ = EnNew(SnParagraph($2, @1));
				} ;

StatementList:	StatementList Statement {
						if ($2 != nullptr)
							$1->push_back($2);
						$$ = $1;
					} |
					{
						//on empty
						$$ = EnNew(PtrList<SnStatement>());
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
				Paragraph {
					$$ = $1;
				} |
				error ';'
				{
					parser.Log(CLL_Error, @1, "Invalid statement.");
					$$ = nullptr;
				} ;

ReturnStmt: KT_Return Expression ';' {
					$$ = EnNew(SnReturnStmt($2, @1));
				} ;

InvokeStmt: InvokeExpr ';' { $$ = EnNew(SnInvokeStmt($1, @1)); } |
				MemberExpr ';' { $$ = EnNew(SnInvokeStmt($1, @1)); } ;

/*
Local variable declaration statement.
Reference: EN's LocalDeclStmt (compiler_bak/grammer/nlang.y:535).
*/
LocalDeclStmt: Type LocalDeclList ';' {
						$$ = EnNew(SnLocalDeclStmt($1, $2, @1));
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
						$$ = EnNew(std::vector<SnLocalDeclStmt::LocalDecl>());
						$$->push_back({*$1, $3});
					} |
					TT_Identifier {
						$$ = EnNew(std::vector<SnLocalDeclStmt::LocalDecl>());
						$$->push_back({*$1, nullptr});
					} ;

/*
Assignment statement.
Reference: EN's AssignStmt (compiler_bak/grammer/nlang.y:546).
*/
AssignStmt: IdentifierExpr '=' Expression ';' {
					$$ = EnNew(SnAssignStmt($1, $3, @1));
				} |
				MemberExpr '=' Expression ';' {
					$$ = EnNew(SnAssignStmt($1, $3, @1));
				} ;

/*
Array subscript assignment statement (e.g. arr[i] = value).
*/
SubscriptAssignStmt: Expression '[' Expression ']' '=' Expression ';' {
					$$ = EnNew(SnSubscriptAssignStmt($1, $3, $6, @1));
				} ;

/*
If/else statement.
Reference: EN's IfStmt (compiler_bak/grammer/nlang.y:554).
*/
IfStmt:	KT_If '(' Expression ')' Statement %prec P_Then {
				$$ = EnNew(SnIfStmt($3, $5, nullptr, @1));
			} |
			KT_If '(' Expression ')' Statement KT_Else Statement {
				$$ = EnNew(SnIfStmt($3, $5, $7, @1));
			} ;


/*
While loop statement.
Reference: EN's WhileStmt (compiler_bak/grammer/nlang.y:561).
*/
WhileStmt:	KT_While '(' Expression ')' Statement {
					$$ = EnNew(SnWhileStmt($3, $5, @1));
				} ;

/*
Do-while loop statement.
Reference: EN's DoStmt (compiler_bak/grammer/nlang.y:565).
*/
DoStmt:	KT_Do Statement KT_While '(' Expression ')' ';' {
				$$ = EnNew(SnDoStmt($5, $2, @1));
			} ;

/*
For loop statement.
Reference: EN's ForStmt (compiler_bak/grammer/nlang.y:569).
*/
ForStmt:	KT_For '(' InitFor ';' Expression ';' FiniFor ')' Statement {
					$$ = EnNew(SnForStmt($3, $5, $7, $9, @1));
				} ;

InitFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				Type LocalDeclList {
					$$ = EnNew(SnLocalDeclStmt($1, $2, @1));
				} |
				InvokeExpr {
					$$ = EnNew(SnInvokeStmt($1, @1));
				} |
				IdentifierExpr '=' Expression {
					$$ = EnNew(SnAssignStmt($1, $3, @1));
				} ;

FiniFor:	{
					/* on empty */
					$$ = nullptr;
				} |
				InvokeExpr {
					$$ = EnNew(SnInvokeStmt($1, @1));
				} |
				IdentifierExpr '=' Expression {
					$$ = EnNew(SnAssignStmt($1, $3, @1));
				} ;

/*
Foreach loop statement (Phase 8e-5).
Iterates Array / List<T> / Dict<K,V> (keys).
*/
ForeachStmt:	KT_Foreach '(' Type TT_Identifier KT_In Expression ')' Statement {
					$$ = EnNew(SnForeachStmt($3, *$4, $6, $8, @1));
				} ;

/*
Break statement.
Reference: EN's BreakStmt (compiler_bak/grammer/nlang.y:624).
*/
BreakStmt:	KT_Break ';' {
					$$ = EnNew(SnBreakStmt(@1));
				} ;

/*
Continue statement.
Reference: EN's ContinueStmt (compiler_bak/grammer/nlang.y:628).
*/
ContinueStmt:	KT_Continue ';' {
					$$ = EnNew(SnContinueStmt(@1));
				} ;

/*
Switch statement.
Reference: EN's SwitchStmt (compiler_bak/grammer/nlang.y:598).
*/
SwitchStmt:	KT_Switch '(' Expression ')' '{' CaseClauseList DefaultCase '}' {
				$$ = EnNew(SnSwitchStmt($3, $6, $7, @1));
			} ;

CaseClauseList:	CaseClauseList CaseClause {
					$1->push_back($2);
					$$ = $1;
				} |
				{
					/*on empty */
					$$ = EnNew(std::vector<SnCaseClause*>());
				} ;

CaseClause:	KT_Case Expression ':' StatementList {
				$$ = EnNew(SnCaseClause($2, $4, @1));
			} ;

DefaultCase:	{
					/*on empty */
					$$ = nullptr;
				} |
				KT_Default ':' StatementList {
					$$ = EnNew(SnParagraph($3, @1));
				} ;
/*
Enum type declaration.
*/
EnumDecl:	KT_Enum TT_Identifier '{' EnumMemberList '}' {
					$$ = EnNew(SnEnumDecl($2, $4, @1));
				} ;

EnumMemberList:	EnumMemberList ',' EnumMember {
					$1->push_back($3);
					$$ = $1;
				} |
				EnumMember {
					$$ = EnNew(PtrList<SnEnumMember>());
					$$->push_back($1);
				} ;

EnumMember:	TT_Identifier {
				$$ = EnNew(SnEnumMember($1, nullptr, @1));
				} |
				TT_Identifier '=' Expression {
					$$ = EnNew(SnEnumMember($1, $3, @1));
				} ;

/*
Struct type declaration.
*/
StructDecl:	KT_Struct TT_Identifier '{' StructFieldList '}' {
					$$ = EnNew(SnStructDecl($2, $4, @1));
				} ;

StructFieldList:	StructFieldList StructField {
					$1->push_back($2);
					$$ = $1;
				} |
					StructField {
					$$ = EnNew(PtrList<SnStructField>());
					$$->push_back($1);
				} ;

StructField:	Type TT_Identifier ';' {
					$$ = EnNew(SnStructField($1, $2, @1));
				} ;


/*
Class type declaration.
*/
ClassDecl:	KT_Class TT_Identifier ClassInheritOpt ImplementsOpt '{' ClassMemberList '}' {
					auto* pClass = EnNew(SnClassDecl($2, $3, $6, @1));
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
					$$ = EnNew(PtrList<SnFieldExpr>());
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
					$$ = EnNew(SnInterfaceDecl($2, $4, @1));
				} ;

InterfaceMemberList:	InterfaceMemberList InterfaceMember {
					$1->push_back($2);
					$$ = $1;
				} |
				InterfaceMember {
					$$ = EnNew(PtrList<SnField>());
					$$->push_back($1);
				} |
				/* empty */ {
					$$ = EnNew(PtrList<SnField>());
				} ;

InterfaceMember:	AccessType NodeFlag Type TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
					func->AddFlags(NF_Abstract);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' ';' {
					auto* func = EnNew(SnFunction($1, NF_NONE, $2, $3, $5, @2));
					func->AddFlags(NF_Abstract);
					$$ = func;
				} ;

ClassMemberList:	ClassMemberList ClassMember {
					$1->push_back($2);
					$$ = $1;
				} |
					ClassMember {
					$$ = EnNew(PtrList<SnField>());
					$$->push_back($1);
				} ;

ClassMember:	AccessType NodeFlag Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				} |
				AccessType NodeFlags Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = EnNew(SnFunction($1, $2, $3, $4, $6, @2));
					if ($8 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($8);
					$$ = func;
				} |
				AccessType Type TT_Identifier '(' FormalParamList ')' FunctionBodyOrSemi {
					auto* func = EnNew(SnFunction($1, NF_NONE, $2, $3, $5, @2));
					if ($7 == nullptr)
						func->AddFlags(NF_Abstract);
					else
						func->Body($7);
					$$ = func;
				} |

				AccessType Type TT_Identifier ';' {
					$$ = EnNew(SnClassField($2, $3, $1, @1));
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

NameExpr:	IdentifierExpr	{ $$ = EnNew(SnNameExpr($1, @1)); } |
				MemberExpr		{ $$ = EnNew(SnNameExpr($1, @1)); } ;

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
				NameExpr '<' TypeList '>'	{ $$ = EnNew(SnGenericTypeExpr($1, $3, @1)); } |
				Type OT_Brackets	{ $$ = EnNew(SnArrayTypeExpr($1, @2)); } ;

//Comma-separated list of type arguments inside `<...>`. Used only by
//the generic Type rule above.
TypeList:	Type {
						$$ = new std::vector<nlang::SnFieldExpr*>{ $1 };
					} |
					TypeList ',' Type {
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
				KT_This		{ $$ = EnNew(SnThisExpr(@1)); } |
				KT_Null		{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(), 0, @1)); } |
				//Phase 8e-6: bare `[...]` array/list literal. Only the bracket
				//form is allowed bare; dict/struct `{...}` requires explicit
				//`new Type{...}` because bare `{...}` would LALR-conflict
				//with Paragraph (block statement).
				'[' InitListElements ']'	{ $$ = EnNew(SnInitListExpr(nullptr, *$2, true, @1)); delete $2; } |
				Expression '+' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Add, $1, $3, @1)); } |
				Expression '-' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Sub, $1, $3, @1)); } |
				Expression '*' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Mul, $1, $3, @1)); } |
				Expression '/' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Div, $1, $3, @1)); } |
				Expression '%' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Mod, $1, $3, @1)); } |
				Expression '<' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Less, $1, $3, @1)); } |
				Expression OT_LE Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_LessEqual, $1, $3, @1)); } |
				Expression '>' Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Greater, $1, $3, @1)); } |
				Expression OT_GE Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_GreaterEqual, $1, $3, @1)); } |
				Expression OT_EQ Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Equal, $1, $3, @1)); } |
				Expression OT_NE Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_NotEqual, $1, $3, @1)); } |
				Expression OT_AND Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_LogicalAnd, $1, $3, @1)); } |
				Expression OT_OR Expression	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_LogicalOr, $1, $3, @1)); } |
				'-' Expression %prec P_Minus	{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_Neg, $2, @1)); } |
				'!' Expression					{ $$ = EnNew(SnBinaryExpr(SnBinaryExpr::OP_LogicalNot, $2, @1)); } |
				Expression KT_As NameExpr		{ $$ = EnNew(SnAsExpr($1, $3, @2)); } ;

ParenthesesExpr: '(' Expression ')' { $$ = $2; } ;

MemberExpr:	Expression '.' InvokeExpr		{
					$$ = EnNew(SnMemberExpr($1, $3, @1));
				} |
				Expression '.' IdentifierExpr	{
					$$ = EnNew(SnMemberExpr($1, $3, @1));
				} ;

LiteralExpr:	TT_Int		{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	$1,	@1));	} |
					TT_UInt		{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1));	} |
					TT_Short	{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1));	} |
					TT_UShort	{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1));	} |
					TT_Byte		{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1));	} |
					TT_UByte	{ $$ = EnNew(SnLiteralExpr(*RnInt32::Instance(),	static_cast<int32>($1),	@1));	} |
					TT_Float	{ $$ = EnNew(SnLiteralExpr(*RnFloat::Instance(),	$1,	@1));	} |
					TT_String 	{ $$ = EnNew(SnLiteralExpr(*RnString::Instance(),	$1,	@1));	} ;

InvokeExpr:	TT_Identifier '(' ConcreteParamList ')' {
					$$ = EnNew(SnInvokeExpr($1, $3, @1));
				} ;

IdentifierExpr:	TT_Identifier	{ $$ = EnNew(SnIdentifierExpr($1, @1));			} |
					KT_Int   		{ $$ = EnNew(SnIdentifierExpr(NK_Int32, @1));	} |
					KT_Float		{ $$ = EnNew(SnIdentifierExpr(NK_Float, @1));	} |
					KT_String		{ $$ = EnNew(SnIdentifierExpr(NK_String, @1));	} ;

NewExpr:	KT_New TT_Identifier '(' ConcreteParamList ')' {
					auto* pId = EnNew(SnIdentifierExpr($2, @2));
					$$ = EnNew(SnNewExpr(EnNew(SnNameExpr(pId, @2)), $4, @1));
				} |
				//Phase 8e-3: generic construction `new List<int>()`.
				//Separate rule to avoid touching the plain `new Foo()` parse
				//(which has its own LALR state). $2 is the base class name;
				//$4 is the type-args list; $7 is the constructor args.
				KT_New TT_Identifier '<' TypeList '>' '(' ConcreteParamList ')' {
					auto* pId = EnNew(SnIdentifierExpr($2, @2));
					auto* pName = EnNew(SnNameExpr(pId, @2));
					$$ = EnNew(SnNewExpr(EnNew(SnGenericTypeExpr(pName, $4, @2)), $7, @1));
				} |
				//Phase 8e-6: explicit collection init `new Foo{...}` /
				//`new List<int>{...}`. Initializes a fresh instance with
				//the given entries (dict key:value pairs or struct fields).
				KT_New TT_Identifier '{' InitEntries '}' {
					auto* pId = EnNew(SnIdentifierExpr($2, @2));
					auto* pName = EnNew(SnNameExpr(pId, @2));
					$$ = EnNew(SnInitListExpr(pName, *$4, false, @1));
					delete $4;
				} |
				KT_New TT_Identifier '<' TypeList '>' '{' InitEntries '}' {
					auto* pId = EnNew(SnIdentifierExpr($2, @2));
					auto* pName = EnNew(SnNameExpr(pId, @2));
					$$ = EnNew(SnInitListExpr(
						EnNew(SnGenericTypeExpr(pName, $4, @2)), *$7, false, @1));
					delete $7;
				} ;

NewArrayExpr:	KT_New Type '[' Expression ']' {
					$$ = EnNew(SnNewArrayExpr($2, $4, @1));
				} ;

SubscriptExpr:	Expression '[' Expression ']' {
					$$ = EnNew(SnSubscriptExpr($1, $3, @1));
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
					} ;

ConcreteParamList:	ConcreteParamList ',' Expression {
							if ($1->empty())
							{
								parser.Log(CLL_Error, @2, "Invalid concrete param list, "
									"expecte param before ','.");

							}
							else
								$1->push_back($3);
							$$ = $1;
						} |
						Expression {
							$$ = EnNew(PtrList<SnExpression>());
							$$->push_back($1);
						} |
						{
							//on empty
							$$ = EnNew(PtrList<SnExpression>());
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
