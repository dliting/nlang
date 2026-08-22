/* flex version = 2.6.4 */
%option nounistd
%option nodefault noyywrap yylineno reentrant
%option bison-bridge bison-locations
%option header-file="nlang.lex.h"
%option never-interactive

/* Include stdint.h before flex defines its own INT8_MIN etc. macros.
   MSVC does not define __STDC_VERSION__ in C++ mode, causing flex to
   enter the non-C99 branch and define conflicting macros. */
%top{
#include <stdint.h>
}

%{

#include "nlang.tab.h"
#include "ScriptScanner.h"
#include <cassert>
#include <cmath>
#include <cstring>

/* handle locations */
#define YY_USER_ACTION														\
do {																		\
    yylloc->m_pTransUnit = scanner.TransUnit();								\
    if (!bConcatToken)														\
    {																		\
        yylloc->m_nStartLine    = yylineno;									\
        yylloc->m_nStartCol     = yycolumn + 1;								\
    }																		\
    yycolumn += yyleng;														\
    yylloc->m_nEndLine  = yylineno;											\
    yylloc->m_nEndCol   = yycolumn;											\
} while(0);																	


#define BEGIN_STATE(s)														\
do {																		\
    BEGIN(s);																\
    scanner.StartState(s);														\
} while(0)

#define APPEND_TEXT(s)														\
do {																		\
    if (scanner.ScanType() == nlang::ScriptScanner::CT_Compiler)							\
    {																		\
        yylval->v_pStr->append(s);											\
    }																		\
} while(0)

#define SET_TEXT(s)															\
do {																		\
    if (scanner.ScanType() == nlang::ScriptScanner::CT_Compiler)							\
    {																		\
        yylval->v_pStr = EnNew(std::string(s));								\
    }																		\
} while(0)

#define FREE_TEXT()															\
do {																		\
    if (scanner.ScanType() == nlang::ScriptScanner::CT_Compiler)							\
    {																		\
        EnDelete(yylval->v_pStr);											\
    }																		\
} while(0)

#define DEFER_EOF(deferToken, deferState)									\
do {																		\
    if (!scanner.DeferEof())												\
    {																		\
        scanner.DeferEof(true);												\
        return deferToken;													\
    }																		\
    else																	\
    {																		\
        BEGIN_STATE(deferState);											\
        scanner.DeferEof(false);											\
        return 0;															\
    }																		\
} while(0)


#define DEAL_INT_TOKEN(n)													\
do {																		\
    if (n >= -128)															\
    {																		\
        yylval->v_Byte = n;													\
        return TT_Byte;														\
    }																		\
    else if (n >= -65536)													\
    {																		\
        yylval->v_Short = n;												\
        return TT_Short;													\
    }																		\
    else																	\
    {																		\
        yylval->v_Int = n;													\
        return TT_Int;														\
    }																		\
} while(0)																	


#define DEAL_UINT_TOKEN(n)													\
do {																		\
    if (n < 128)															\
    {																		\
        yylval->v_UByte = n;												\
        return TT_UByte;													\
    }																		\
    else if (n < 65536)														\
    {																		\
        yylval->v_UShort = n;												\
        return TT_UShort;													\
    }																		\
    else																	\
    {																		\
        yylval->v_UInt = n;													\
        return TT_UInt;														\
    }																		\
} while(0)																	

%}
%option extra-type="nlang::ScriptScanner*"

%x COMMENT_S
%x STRING_S

Blanks  	[ \t]+
Alpha		[_A-Za-z]
DecDigit	[0-9]
HexDigit    {DecDigit}[a-fA-F]
DecInt      [-+]?{DecDigit}+([eE][-+]?{DecDigit}+)?
HexInt      [-+]?(0x|0X){HexDigit}+
Float       [-+]?{DecDigit}*"."{DecDigit}+([eE][-+]?{DecDigit}+)?
Name		{Alpha}({Alpha}|{DecDigit})*
LineComment	\/\/[^\n]*

%%
	#define scanner (*yyextra)
	bool bConcatToken = false;
	BEGIN(scanner.StartState());

{Blanks}    /* skip blanks and tabs */

            /* keywords */
"bool"      { return KT_Bool; }
"as"        { return KT_As; }
"assert"    { return KT_Assert; }
"break"     { return KT_Break; }
"byte"      { return KT_Byte; }
"case"      { return KT_Case; }
"catch"     { return KT_Catch; }
"char"      { return KT_Char; }
"class"     { return KT_Class; }
"const"     { return KT_Const; }
"continue"  { return KT_Continue; }
"do"        { return KT_Do; }
"default"   { return KT_Default; }
"else"      { return KT_Else; }
"elseif"    { return KT_Elseif; }
"enum"      { return KT_Enum; }
"false"     { return KT_False; }
"finally"   { return KT_Finally; }
"float"     { return KT_Float; }
"for"       { return KT_For; }
"foreach"   { return KT_Foreach; }
"if"        { return KT_If; }
"implements" { return KT_Implements; }
"import"    { return KT_Import; }
"in"        { return KT_In; }
"int"       { return KT_Int; }
"interface" { return KT_Interface; }
"namespace" { return KT_Namespace; }
"native"    { return KT_Native; }
"new"       { return KT_New; }
"null"      { return KT_Null; }
"out"       { return KT_Out; }
"private"   { return KT_Private; }
"protected" { return KT_Protected; }
"public"    { return KT_Public; }
"return"    { return KT_Return; }
"short"     { return KT_Short; }
"state"     { return KT_State; }
"static"    { return KT_Static; }
"string"    { return KT_String; }
"struct"    { return KT_Struct; }
"super"     { return KT_Super; }
"switch"    { return KT_Switch; }
"this"      { return KT_This; }
"throw"     { return KT_Throw; }
"true"      { return KT_True; }
"try"       { return KT_Try; }
"ubyte"     { return KT_Ubyte; }
"uint"      { return KT_Uint; }
"ushort"    { return KT_Ushort; }
"using"     { return KT_Using; }
"virtual"   { return KT_Virtual; }
"void"      { return KT_Void; }
"while"     { return KT_While; }

            /* double-character operators */
"++"        { return OT_INC; }
"--"        { return OT_DEC; }
"<<"        { return OT_LSH; }
">>"        {
                /* C#-style split: while closing type-argument lists,
                 * ">>" must reach the parser as two '>' tokens so
                 * `List<List<int>>` parses. yyless(1) rescans the
                 * second '>' through the single-'>' rule below, which
                 * decrements the depth again (so ">>>" closes three
                 * levels via repeated re-matching). */
                if (scanner.GenericDepth() > 0)
                {
                    int n = yyleng;
                    yyless(1);
                    /* YY_USER_ACTION already advanced yycolumn by n;
                     * the rescan adds one more, so compensate to keep
                     * column tracking exact. */
                    yycolumn -= n - 1;
                    scanner.DecGenericDepth();
                    return '>';
                }
                return OT_RSH;
            }
"<="        { return OT_LE; }
">="        { return OT_GE; }
"!="        { return OT_NE; }
"=="        { return OT_EQ; }
"~="        { return OT_AEQ; }
"&&"        { return OT_AND; }
"||"        { return OT_OR; }
"%="        { return OT_MODS; }
"&="        { return OT_BANDS; }
"*="        { return OT_MULS; }
"+="        { return OT_INCS; }
"-="        { return OT_DECS; }
"/="        { return OT_DIVS; }
"|="        { return OT_BORS; }

            /* single-char angle brackets with generic-depth tracking */
"<"         {
                /* '<' opens a type-argument list only when it directly
                 * follows a built-in generic name (List/Dict), flagged
                 * by the identifier rule below. Any other '<' is a
                 * comparison and leaves the depth alone. */
                if (scanner.TakePendingGenericOpen())
                    scanner.IncGenericDepth();
                return '<';
            }
">"         {
                scanner.DecGenericDepth();
                return '>';
            }

\[\]        { return OT_Brackets; }

{Name}      {
                SET_TEXT(yytext);
                /* Flag List/Dict so a following '<' opens a type-argument
                 * list (see the '<' rule). Every identifier rewrites this
                 * flag, so it can only reach a '<' across blanks/comments
                 * — a comparison `List < 3` against a variable shadowing
                 * the type name is misread (documented limitation). */
                scanner.PendingGenericOpen(
                    strcmp(yytext, "List") == 0
                    || strcmp(yytext, "Dict") == 0);
                return TT_Identifier;
            }

{Float}     {
                yylval->v_Float = static_cast<float>(atof(yytext));
                return TT_Float;
            }

{DecInt}    {
                /*Full-value parse. The pattern admits exponent forms
                 *("2e5"), and sscanf("%d") silently stops at 'e' (the
                 *whole literal would read as 2). atof applies the
                 *exponent; values that are not integral or fall outside
                 *the 32-bit token range (int32 signed / uint32 unsigned)
                 *are rejected as TT_Error — same channel as the unknown
                 *string-escape rejection. */
                double full = atof(yytext);
                if (yytext[0] == '-')
                {
                    if (full < INT32_MIN || full != std::floor(full))
                        return TT_Error;
                    nlang::int32 n;
                    n = (nlang::int32)full;
                    DEAL_INT_TOKEN(n);
                }
                else
                {
                    if (full > UINT32_MAX || full != std::floor(full))
                        return TT_Error;
                    nlang::uint32 n;
                    n = (nlang::uint32)full;
                    DEAL_UINT_TOKEN(n);
                }
            }

{HexInt}    {
                if (yytext[0] == '-')
                {
                    nlang::int32 n;
                    sscanf(yytext, "%x", &n);
                    DEAL_INT_TOKEN(n);
                }
                else
                {
                    nlang::uint32 n;
                    sscanf(yytext, "%x", &n);
                    DEAL_UINT_TOKEN(n);
                }
            }

'[^'\n\r\t\\]'   {
                yylval->v_Char = yytext[1];
                return TT_Char;
            }

'\''       {
                yylval->v_Char = '\'';
                return TT_Char;
            }

'\\n'       {
                yylval->v_Char = '\n';
                return TT_Char;
            }

'\\r'       {
                yylval->v_Char = '\r';
                return TT_Char;
            }

'\\t'       {
                yylval->v_Char = '\t';
                return TT_Char;
            }

'\\\\'       {
                yylval->v_Char = '\\';
                return TT_Char;
            }

{LineComment}	{
                if (scanner.ScanType() == nlang::ScriptScanner::CT_Editor)
                    return TT_Comment;
            }

"/*"		{
                bConcatToken = true;
                BEGIN_STATE(COMMENT_S);
            }

\"			{
                bConcatToken = true;
                SET_TEXT("");
                BEGIN_STATE(STRING_S);
            }

\n          {
                scanner.ResetCol();
            }

<INITIAL><<EOF>>  {
                return 0;
            }

<INITIAL>.  {
                return yytext[0];
            }

<COMMENT_S>"*/"	{
                //The comment is complete: the next token must own its
                //start location (compiler diagnostics point at the token,
                //not at the comment). Same idiom as the STRING_S closing
                //rule; the returned TT_Comment keeps the span opened at
                //"/*" because YY_USER_ACTION has already run.
                bConcatToken = false;
                BEGIN_STATE(INITIAL);
                if (scanner.ScanType() == nlang::ScriptScanner::CT_Editor)
                    return TT_Comment;
            }

<COMMENT_S>\n	{
                //Restart column tracking on each comment line so tokens on
                //the closing line carry real columns; keep concatenating
                //(the comment token still spans). Raw yycolumn only --
                //ResetCol would stomp the in-progress comment location.
                yycolumn = 0;
                bConcatToken = true;
            }

<COMMENT_S>([^*\n]*)|("*"+[^*/\n]*)	{
                bConcatToken = true;
            }

<COMMENT_S><<EOF>>	{
                if (scanner.ScanType() == nlang::ScriptScanner::CT_Editor)
                    DEFER_EOF(TT_Comment, COMMENT_S);
                else
                    DEFER_EOF(TT_Error, INITIAL);
            }

<STRING_S>"\\n"	{
                APPEND_TEXT("\n");
            }

<STRING_S>"\\r"	{
                APPEND_TEXT("\r");
            }

<STRING_S>"\\t" {
                APPEND_TEXT("\t");
            }

<STRING_S>"\\\""    {
                APPEND_TEXT("\"");
            }

<STRING_S>"\\\\"	{
                APPEND_TEXT("\\");
            }

<STRING_S>"\\'"	{
                APPEND_TEXT("'");
            }

<STRING_S>"\\0"	{
                APPEND_TEXT(std::string(1, '\0'));
            }

<STRING_S>"\\a"	{
                APPEND_TEXT(std::string(1, '\a'));
            }

<STRING_S>"\\b"	{
                APPEND_TEXT(std::string(1, '\b'));
            }

<STRING_S>"\\f"	{
                APPEND_TEXT(std::string(1, '\f'));
            }

<STRING_S>"\\v"	{
                APPEND_TEXT(std::string(1, '\v'));
            }

<STRING_S>"\\".    {
                //Unknown escape — reject instead of silently emitting a
                //literal backslash (the pre-fix catch-all behavior).
                FREE_TEXT();
                bConcatToken = false;
                BEGIN_STATE(INITIAL);
                return TT_Error;
            }

<STRING_S>\n	{
                FREE_TEXT();
                bConcatToken = false;
                BEGIN_STATE(INITIAL);
                return TT_Error;
            }

<STRING_S>\"	{
                //APPEND_TEXT(yytext);
                bConcatToken = false;
                BEGIN_STATE(INITIAL);
                return TT_String;
            }

<STRING_S><<EOF>>   {
                DEFER_EOF(TT_Error, INITIAL);
            }

<STRING_S>. {
                APPEND_TEXT(yytext);
            }

%%

