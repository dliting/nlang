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
#include <nlang/runtime/BuiltinGenericNames.h>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <nlang/runtime/PrimitiveTypes.h>  // Utf8EncodeCodePoint (0.7.5)

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
        yylval->v_pStr = new std::string(s);								\
    }																		\
} while(0)

#define FREE_TEXT()															\
do {																		\
    if (scanner.ScanType() == nlang::ScriptScanner::CT_Compiler)							\
    {																		\
        delete yylval->v_pStr;											\
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


/*0.7.5 value-range tiering: <=INT32_MAX stays int, then long, then
  ulong — the five narrow tokens retired (constant-fit at the cast
  site replaces lexical tiering into byte/short). Unsigned-only:
  literal signs are retired ('-'/'+' are unary operators), so the
  signed DEAL tier is dead and removed with them.*/
#define DEAL_UINT_TOKEN(n)													\
do {																		\
    if (n <= INT32_MAX)														\
    {																		\
        yylval->v_Int = (nlang::int32)n;									\
        return TT_Int;														\
    }																		\
    if (n <= INT64_MAX)														\
    {																		\
        yylval->v_Long = (nlang::int64)n;									\
        return TT_Long;														\
    }																		\
    yylval->v_ULong = n;													\
    return TT_ULong;														\
} while(0)


/*0.7.5 char/string \uXXXX support. Hex value of exactly 4
  digits; the pattern side has already bounded the shape. */
static unsigned ParseHex4(const char* p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; ++i)
    {
        const char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= unsigned(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= unsigned(c - 'a' + 10);
        else
            v |= unsigned(c - 'A' + 10);
    }
    return v;
}

/*Decode one char-literal body (the bytes between the quotes) into a
  Unicode scalar value. The flex pattern bounds the byte span; this
  action enforces the semantics: a bare segment must decode to exactly
  one UTF-8 code point (minimal length, non-surrogate, <= 0x10FFFF),
  escapes resolve by table, \uXXXX is BMP non-surrogate only. */
static bool DecodeCharLiteral(const char* p, int len, nlang::nchar& out)
{
    if (len >= 2 && p[0] == '\\')
    {
        if (len == 2)
        {
            switch (p[1])
            {
            case 'n':   out = 0x0A; return true;
            case 'r':   out = 0x0D; return true;
            case 't':   out = 0x09; return true;
            case '\'':   out = 0x27; return true;
            case '\\':  out = 0x5C; return true;
            default:    return false;
            }
        }
        if (len == 6 && p[1] == 'u')
        {
            const unsigned v = ParseHex4(p + 2);
            if (v >= 0xD800 && v <= 0xDFFF)
                return false;
            out = nlang::nchar(v);
            return true;
        }
        return false;
    }
    if (len < 1 || len > 4)
        return false;
    const unsigned char b0 = (unsigned char)p[0];
    unsigned cp;
    int n;
    if (b0 < 0x80)                   { cp = b0;          n = 1; }
    else if ((b0 & 0xE0) == 0xC0)    { cp = b0 & 0x1F;   n = 2; }
    else if ((b0 & 0xF0) == 0xE0)    { cp = b0 & 0x0F;   n = 3; }
    else if ((b0 & 0xF8) == 0xF0)    { cp = b0 & 0x07;   n = 4; }
    else                             { return false; }
    if (len != n)
        return false;
    for (int i = 1; i < n; ++i)
    {
        const unsigned char bi = (unsigned char)p[i];
        if ((bi & 0xC0) != 0x80)
            return false;
        cp = (cp << 6) | (bi & 0x3F);
    }
    static const unsigned kMinCp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    if (cp < kMinCp[n] || cp > 0x10FFFF)
        return false;
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return false;
    out = nlang::nchar(cp);
    return true;
}

%}
%option extra-type="nlang::ScriptScanner*"

%x COMMENT_S
%x STRING_S

Blanks  	[ \t]+
Alpha		[_A-Za-z]
DecDigit	[0-9]
HexDigit    ({DecDigit}|[a-fA-F])
DecInt      {DecDigit}+
HexInt      (0x|0X){HexDigit}+
Float       {DecDigit}*"."{DecDigit}+([eE][-+]?{DecDigit}+)?[fF]?|{DecDigit}+[eE][-+]?{DecDigit}+[fF]?
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
"double"    { return KT_Double; }
"else"      { return KT_Else; }
"elseif"    { return KT_Elseif; }
"enum"      { return KT_Enum; }
"false"     { return KT_False; }
"finally"   { return KT_Finally; }
"float"     { return KT_Float; }
"for"       { return KT_For; }
"foreach"   { return KT_Foreach; }
"func"      {
                /* 0.8.3: keyword form of the function type. Same
                 * pending-open flag as the List/Dict identifier rule
                 * so `func<...>` opens a type-argument list (the '<'
                 * rule consumes the flag; ">>" splits rely on the
                 * depth staying balanced). */
                scanner.PendingGenericOpen(true);
                return KT_Func;
            }
"if"        { return KT_If; }
"implements" { return KT_Implements; }
"import"    { return KT_Import; }
"in"        { return KT_In; }
"int"       { return KT_Int; }
"interface" { return KT_Interface; }
"long"      { return KT_Long; }
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
"ulong"     { return KT_ULong; }
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
                 * follows a built-in generic name (List/Dict via the
                 * identifier rule below; func via its keyword rule).
                 * Any other '<' is a comparison and leaves the depth
                 * alone. */
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
                /* Flag List/Dict so a following '<' opens a
                 * type-argument list (see the '<' rule). Every identifier
                 * rewrites this flag, so it can only reach a '<' across
                 * blanks/comments — a comparison `List < 3` against a
                 * variable shadowing the type name is misread (documented
                 * limitation). */
                /*Flex action code runs in the global namespace, so the
                 *built-in name constants are qualified with nlang::
                 *(they live in the runtime namespace). */
                scanner.PendingGenericOpen(
                    strcmp(yytext, nlang::kBuiltinListTypeName) == 0
                    || strcmp(yytext, nlang::kBuiltinDictTypeName) == 0);
                return TT_Identifier;
            }

{Float}     {
                /*0.7.5: an f/F suffix selects the float carrier, an
                 *unsuffixed literal (dot or exponent form) is double.
                 *The pattern validated the shape, so pEnd lands on
                 *the suffix or the NUL. */
                char* pEnd = NULL;
                double full = strtod(yytext, &pEnd);
                if (*pEnd == 'f' || *pEnd == 'F')
                {
                    yylval->v_Float = static_cast<float>(full);
                    return TT_Float;
                }
                yylval->v_Double = full;
                return TT_Double;
            }

{DecInt}    {
                /*Full-value 64-bit parse (0.7.5): literals are
                 *unsigned (signs are unary operators), exponent forms
                 *live in {Float}; beyond UINT64_MAX is a lexical
                 *error. */
                errno = 0;
                nlang::uint64 n = strtoull(yytext, NULL, 10);
                if (errno == ERANGE) return TT_Error;
                DEAL_UINT_TOKEN(n);
            }

{HexInt}    {
                /*Full-value 64-bit parse (0.7.5), unsigned-only like
                 *DecInt. */
                errno = 0;
                nlang::uint64 n = strtoull(yytext, NULL, 16);
                if (errno == ERANGE) return TT_Error;
                DEAL_UINT_TOKEN(n);
            }

'([^'\n\r\t\\]{1,4}|\\u[0-9a-fA-F]{4}|\\[nrt'\\])'   {
                //0.7.5: one rule for the whole char-literal family.
                //Flex bracket classes match BYTES, so {1,4} covers
                //any single UTF-8 code point (1-4 bytes) without
                //scanner state; this action then enforces "exactly
                //one Unicode scalar value" (rejects multi-point
                //spans, truncated sequences, surrogate \uXXXX).
                nlang::nchar cp;
                if (!DecodeCharLiteral(yytext + 1, yyleng - 2, cp))
                    return TT_Error;
                yylval->v_Char = cp;
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

<STRING_S>"\\u"[dD][89aAbB][0-9a-fA-F]{2}"\\u"[dD][cdefCDEF][0-9a-fA-F]{2}	{
                //0.7.5: adjacent surrogate pair (Java-style) combines
                //into one non-BMP code point (4 UTF-8 bytes). Longer
                //than the singleton rule, so longest-match picks it.
                const unsigned hi = ParseHex4(yytext + 2);
                const unsigned lo = ParseHex4(yytext + 8);
                const unsigned cp = 0x10000u
                    + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
                APPEND_TEXT(nlang::Utf8EncodeCodePoint(cp));
            }

<STRING_S>"\\u"[0-9a-fA-F]{4}	{
                //0.7.5: \uXXXX - BMP only; a lone surrogate is an
                //error (pairing requires adjacency, matched above).
                const unsigned cp = ParseHex4(yytext + 2);
                if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    FREE_TEXT();
                    bConcatToken = false;
                    BEGIN_STATE(INITIAL);
                    return TT_Error;
                }
                APPEND_TEXT(nlang::Utf8EncodeCodePoint(cp));
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

