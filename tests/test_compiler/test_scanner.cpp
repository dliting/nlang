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
#include "nlang/compiler/Utf8.h"
#include "nlang.tab.h"

#include <cstdio>
#include <cstring>
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
//always KT_Int, TT_Int (unsigned 5 fits int32 under the tiered
//literal rules), ';'.
static void CheckTokenTriple(const std::vector<TokenPos>& tokens) {
    CHECK(tokens.size() == 3);
    if (tokens.size() != 3)
        return;
    CHECK(tokens[0].type == KT_Int);
    CHECK(tokens[1].type == TT_Int);
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

//0.7.5 literal tiering: a dot-or-exponent form without a suffix is
//double (TT_Double); the f/F suffix selects the float carrier
//(TT_Float). Signs are unary operators, so every fixture is unsigned.
static void TestFloatDoubleTiering() {
    std::printf("float/double tiering\n");
    auto bare = ScanAll("2.5; 2.5e3; 1e30; 2e-1;");
    CHECK(bare.size() == 8);
    if (bare.size() == 8) {
        CHECK(bare[0].type == TT_Double);  //dot form
        CHECK(bare[2].type == TT_Double);  //dot+exponent
        CHECK(bare[4].type == TT_Double);  //exponent only
        CHECK(bare[6].type == TT_Double);  //negative exponent
    }
    auto suffixed = ScanAll("2.5f; 1.5e3F;");
    CHECK(suffixed.size() == 4);
    if (suffixed.size() == 4) {
        CHECK(suffixed[0].type == TT_Float);
        CHECK(suffixed[2].type == TT_Float);
    }
}

//0.7.6 UTF-8 input contract: Utf8Valid is the strict shared validator
//(well-formed UTF-8 only — overlongs, surrogates, > U+10FFFF, stray or
//truncated continuations all fail).
static void TestUtf8Validator() {
    std::printf("utf-8 validator\n");
    struct Case { const char* name; const char* bytes; size_t len; bool valid; };
    //"\xe4\xb8\xad" = U+4E2D, "\xf0\x9f\x98\x80" = U+1F600.
    Case cases[] = {
        {"ascii", "int 5;", 5, true},
        {"2byte", "\x41\xe4\xb8\xad", 4, true},
        {"4byte-emoji", "\xf0\x9f\x98\x80", 4, true},
        {"empty", "", 0, true},
        {"stray-continuation", "\x80", 1, false},
        {"truncated-2byte", "\xc2\x41", 2, false},
        {"truncated-tail", "ab\xc3", 3, false},
        {"overlong", "\xc0\xaf", 2, false},
        {"overlong-3byte", "\xe0\x80\xaf", 3, false},
        {"surrogate", "\xed\xa0\x80", 3, false},
        {"above-10ffff", "\xf4\x90\x80\x80", 4, false},
        {"5byte-leader", "\xf8\x88\x80\x80\x80", 5, false},
    };
    for (const Case& c : cases) {
        bool got = nlang::Utf8Valid(c.bytes, c.len);
        if (got != c.valid) {
            std::printf("  FAIL %s: expected %d got %d\n",
                        c.name, c.valid ? 1 : 0, got ? 1 : 0);
            ++g_failures;
        }
    }
}

//Utf8ContentCheck layering on top of the validator: the BOM is skipped,
//content points past it, and both failure shapes carry a diagnosis
//without the file name (the caller knows the noun and the path).
static void TestUtf8ContentCheck() {
    std::printf("utf-8 content check\n");
    static const char kBom[] = "\xef\xbb\xbf""int;";
    const char* content = nullptr;
    size_t len = 0;
    std::string reason;
    CHECK(nlang::Utf8ContentCheck(kBom, 7, &content, &len, &reason));
    CHECK(reason.empty());
    CHECK(len == 4 && std::strncmp(content, "int;", 4) == 0);

    static const char kGbk[] = "\xd6\xd0\xce\xc4";  //"中文" in GBK
    CHECK(!nlang::Utf8ContentCheck(kGbk, 4, &content, &len, &reason));
    CHECK(reason.find("not valid UTF-8") != std::string::npos);
    CHECK(reason.find("line 1") != std::string::npos);

    static const char kUtf16Le[] = "\xff\xfe\x3d\x00";
    CHECK(!nlang::Utf8ContentCheck(kUtf16Le, 4, &content, &len, &reason));
    CHECK(reason.find("UTF-16") != std::string::npos);
}

//One OpenFile round-trip on a byte-exact temp file. `closeIt` says
//whether the open succeeded (and CloseFile must be called).
static bool WriteTempBytes(const char* name, const void* bytes, size_t len) {
    std::FILE* f = std::fopen(name, "wb");
    if (!f)
        return false;
    bool ok = std::fwrite(bytes, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

//Open a file, drain its tokens, close. An open failure is counted and
//the scan is skipped — scanning a scanner that never opened would
//dereference a null scan info (a regression must read as FAIL, not as
//a crash).
static void ScanFileTokens(nlang::ScriptScanner& scanner, const char* name,
                           std::vector<TokenPos>* tokens) {
    tokens->clear();
    const bool opened = scanner.OpenFile(name);
    CHECK(opened);
    if (!opened)
        return;
    int type;
    while ((type = scanner.NextToken()) > 0) {
        const nlang::ScriptLocation& loc = scanner.Location();
        tokens->push_back({type, loc.m_nStartLine, loc.m_nStartCol});
    }
    scanner.CloseFile();
}

//OpenFile enforces the UTF-8 contract at the file boundary: BOM
//skipped (today it rides into the first token and corrupts it), GBK
//and UTF-16 rejected with a named diagnosis, CRLF normalized to LF
//(the old text-mode fopen did this conversion implicitly).
static void TestOpenFileEncodings() {
    std::printf("open file encodings\n");
    const char* kName = "test_scanner_utf8_tmp.n";
    nlang::ScriptScanner scanner(nlang::ScriptScanner::CT_Compiler);
    std::vector<TokenPos> tokens;

    //Plain UTF-8, no BOM.
    static const char kPlain[] = "int 5;\n";
    CHECK(WriteTempBytes(kName, kPlain, sizeof(kPlain) - 1));
    ScanFileTokens(scanner, kName, &tokens);
    CheckTokenTriple(tokens);
    CHECK(tokens[0].line == 1 && tokens[0].col == 1);

    //UTF-8 BOM: accepted and skipped — "int" still starts at column 1,
    //not column 4 (pin: the BOM used to corrupt the leading token).
    static const char kBomSrc[] = "\xef\xbb\xbf""int 5;\n";
    CHECK(WriteTempBytes(kName, kBomSrc, sizeof(kBomSrc) - 1));
    ScanFileTokens(scanner, kName, &tokens);
    CheckTokenTriple(tokens);
    CHECK(tokens[0].line == 1 && tokens[0].col == 1);

    //GBK bytes: rejected with the named diagnosis, no scan started.
    static const char kGbkSrc[] = "s = \"\xd6\xd0\xce\xc4\";\n";
    CHECK(WriteTempBytes(kName, kGbkSrc, sizeof(kGbkSrc) - 1));
    CHECK(!scanner.OpenFile(kName));
    CHECK(scanner.OpenError().find("not valid UTF-8")
          != std::string::npos);
    CHECK(scanner.OpenError().find(kName) != std::string::npos);

    //UTF-16LE (FF FE): dedicated hint, not a generic byte complaint.
    static const char kUtf16Src[] =
        "\xff\xfe\x69\x00\x6e\x00\x74\x00\x20\x00\x35\x00\x3b\x00";
    CHECK(WriteTempBytes(kName, kUtf16Src, sizeof(kUtf16Src) - 1));
    CHECK(!scanner.OpenFile(kName));
    CHECK(scanner.OpenError().find("UTF-16") != std::string::npos);

    //CRLF line endings: normalized to LF — the second line's "int"
    //lands on line 2 column 1 (a raw \r would shift it).
    static const char kCrlfSrc[] = "int 5;\r\nint 6;\r\n";
    CHECK(WriteTempBytes(kName, kCrlfSrc, sizeof(kCrlfSrc) - 1));
    ScanFileTokens(scanner, kName, &tokens);
    CHECK(tokens.size() == 6);
    if (tokens.size() == 6) {
        CHECK(tokens[3].type == KT_Int);
        CHECK(tokens[3].line == 2 && tokens[3].col == 1);
        CHECK(tokens[4].type == TT_Int && tokens[4].col == 5);
        CHECK(tokens[5].type == ';' && tokens[5].col == 6);
    }

    //A lone CR is a line terminator too — a deliberate superset of the
    //old text-mode conversion, so classic-Mac CR-only endings parse
    //as lines instead of a syntax error.
    static const char kCrSrc[] = "int 5;\rint 6;\r";
    CHECK(WriteTempBytes(kName, kCrSrc, sizeof(kCrSrc) - 1));
    ScanFileTokens(scanner, kName, &tokens);
    CHECK(tokens.size() == 6);
    if (tokens.size() == 6) {
        CHECK(tokens[3].line == 2 && tokens[3].col == 1);
        CHECK(tokens[4].col == 5 && tokens[5].col == 6);
    }

    //A BOM-only file is a valid empty input: it opens and yields no
    //tokens (the downstream "module without main" diagnostics own the
    //rest of that story).
    static const char kBomOnly[] = "\xef\xbb\xbf";
    CHECK(WriteTempBytes(kName, kBomOnly, sizeof(kBomOnly) - 1));
    ScanFileTokens(scanner, kName, &tokens);
    CHECK(tokens.empty());

    //A path that does not exist keeps the plain open-failure shape.
    CHECK(!scanner.OpenFile("test_scanner_no_such_file.n"));
    CHECK(scanner.OpenError().find("Cannot open source file")
          != std::string::npos);

    std::remove(kName);
}

int main() {
    TestColumnsWithoutComment();
    TestColumnsAfterSingleLineComment();
    TestColumnsAfterMultiLineComment();
    TestFloatDoubleTiering();
    TestUtf8Validator();
    TestUtf8ContentCheck();
    TestOpenFileEncodings();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
