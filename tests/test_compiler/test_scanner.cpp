// --- ScriptScanner (compiler mode) location tests ---
// Columns are 1-based byte columns of the input stream. These tests pin
// the invariant that a token's location is its own: the first token after
// a block comment owns its start position (nlang.l "*/" used to leave
// bConcatToken set, so it inherited the comment's start), and comment-
// internal newlines keep columns aligned (the old catch-all let
// YY_USER_ACTION's yycolumn += yyleng stand on top of flex's newline
// reset, drifting the closing line's columns by one).
// Lines are 1-based; OpenString pins yy_bs_lineno to 1 because flex only
// initializes it for FILE buffers.
#include "nlang/compiler/ScriptScanner.h"
#include "nlang.tab.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

//One scanned token: type plus the line/column where it starts.
struct TokenPos {
    int type;
    size_t line;
    size_t col;
};

//Runs one compiler-mode scan. Fixtures use only keyword/number/punctuation
//tokens: identifier tokens allocate a heap string the wrapper cannot hand
//back, and this helper has no way to free it.
static std::vector<TokenPos> ScanAll(const char* zInput) {
    nlang::ScriptScanner scanner(nlang::ScriptScanner::CT_Compiler);
    scanner.OpenString(zInput);
    std::vector<TokenPos> tokens;
    int type;
    while ((type = scanner.NextToken()) > 0) {
        const nlang::ScriptLocation& loc = scanner.Location();
        tokens.push_back({type, loc.m_nStartLine, loc.m_nStartCol});
    }
    scanner.CloseString();
    return tokens;
}

//Every fixture scans "int 5;" plus leading noise; the token triple is
//always KT_Int, TT_UByte (unsigned 5 fits a byte), ';'.
static void CheckTokenTriple(const std::vector<TokenPos>& tokens) {
    CHECK(tokens.size() == 3);
    if (tokens.size() != 3)
        return;
    CHECK(tokens[0].type == KT_Int);
    CHECK(tokens[1].type == TT_UByte);
    CHECK(tokens[2].type == ';');
}

static void TestColumnsWithoutComment() {
    std::printf("no comment\n");
    //"int" starts at col 1, "5" at col 5, ";" at col 6.
    auto tokens = ScanAll("int 5;\n");
    CheckTokenTriple(tokens);
    CHECK(tokens[0].line == 1 && tokens[0].col == 1);
    CHECK(tokens[1].line == 1 && tokens[1].col == 5);
    CHECK(tokens[2].line == 1 && tokens[2].col == 6);
}

static void TestColumnsAfterSingleLineComment() {
    std::printf("single-line comment\n");
    //"*/" ends at col 7, so "int" starts at col 9 — the token's own
    //position, not the comment's start.
    auto tokens = ScanAll("/* c */ int 5;\n");
    CheckTokenTriple(tokens);
    CHECK(tokens[0].line == 1 && tokens[0].col == 9);
    CHECK(tokens[1].line == 1 && tokens[1].col == 13);
    CHECK(tokens[2].line == 1 && tokens[2].col == 14);
}

static void TestColumnsAfterMultiLineComment() {
    std::printf("multi-line comment\n");
    //The block comment spans two lines; tokens on the closing line must
    //carry that line's real columns (int at col 6, 5 at col 10, ; at 11),
    //not columns inherited from the comment start or drifted by the
    //newline handling.
    auto tokens = ScanAll("/* c\nc */ int 5;\n");
    CheckTokenTriple(tokens);
    CHECK(tokens[0].line == 2 && tokens[0].col == 6);
    CHECK(tokens[1].line == 2 && tokens[1].col == 10);
    CHECK(tokens[2].line == 2 && tokens[2].col == 11);
}

int main() {
    TestColumnsWithoutComment();
    TestColumnsAfterSingleLineComment();
    TestColumnsAfterMultiLineComment();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
