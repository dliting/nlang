/*--- test_symbol_index.cpp - SymbolIndex / FindStdLibDir unit tests -----*
 * Loads the real stdlib/*.n tree (no mocks) and pins the indexed shape.
 */
#include "nlang/langservice/SymbolIndex.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace nlang::langservice;

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

#ifndef STDLIB_DIR
#define STDLIB_DIR "."
#endif

static void TestLoadsRealStdLib() {
    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);
    // 48 functions: io 15 (10 natives + 5 typed-reader wrappers), math 25, fs 8.
    CHECK(index.size() == 48);
}

static void TestResolvePrint() {
    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);
    const SymbolInfo* s = index.Resolve("io", "print");
    CHECK(s != nullptr);
    if (!s) return;
    CHECK(s->returnType == "void");
    CHECK(s->native);
    CHECK(s->params.size() == 1);
    if (s->params.size() == 1) {
        CHECK(s->params[0].type == "string");
        CHECK(s->params[0].name == "s");
    }
    CHECK(!s->doc.empty());
    CHECK(!s->filePath.empty());
    CHECK(s->line > 0);
}

static void TestSemanticParamNames() {
    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);

    const SymbolInfo* atan2 = index.Resolve("math", "atan2");
    CHECK(atan2 != nullptr && atan2->params.size() == 2);
    if (atan2 && atan2->params.size() == 2) {
        CHECK(atan2->params[0].name == "y");
        CHECK(atan2->params[1].name == "x");
    }

    const SymbolInfo* writeFile = index.Resolve("io", "writeFile");
    CHECK(writeFile != nullptr && writeFile->params.size() == 2);
    if (writeFile && writeFile->params.size() == 2) {
        CHECK(writeFile->params[0].name == "path");
        CHECK(writeFile->params[1].name == "content");
    }

    const SymbolInfo* readFile = index.Resolve("io", "readFile");
    CHECK(readFile != nullptr && readFile->params.size() == 1);
    if (readFile && readFile->params.size() == 1)
        CHECK(readFile->params[0].name == "path");
}

static void TestCompletion() {
    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);

    const auto fsSyms = index.CompletePackage("fs");
    CHECK(fsSyms.size() == 8);

    const auto namespaces = index.Packages();
    CHECK(namespaces.size() == 3);
    CHECK(namespaces[0] == "fs");
    CHECK(namespaces[1] == "io");
    CHECK(namespaces[2] == "math");

    CHECK(index.Resolve("io", "noSuchFn") == nullptr);
    CHECK(index.Resolve("nope", "print") == nullptr);
}

static void TestNlangFunctionIsNotNative() {
    // A function declared without 'native' must index as native=false.
    // The file is bare source now (no wrapper): the package comes from
    // the file stem "mylib".
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_nonnative";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "mylib.n")
        << "// Add two ints.\n"
        << "int add(int a, int b) {\n"
        << "    return a + b;\n"
        << "}\n";

    SymbolIndex index;
    index.LoadFile((tmp / "mylib.n").string());
    const SymbolInfo* add = index.Resolve("mylib", "add");
    CHECK(add != nullptr);
    if (add) {
        CHECK(!add->native);
        CHECK(add->returnType == "int");
        CHECK(add->params.size() == 2);
    }
    fs::remove_all(tmp);
}

static void TestPackageComesFromFilePath() {
    // The scanner reads no in-file head anymore (phase 5 removed the
    // shell syntax): the package is the file's path stem. A legacy file
    // that still wraps declarations in a `namespace fake` block indexes
    // nothing from inside the braces (a braced top-level block is a
    // scope the scanner skips) and the head name must not mint a second
    // package; top-level declarations index under the stem.
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_pkgstem";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "mylib.n")
        << "namespace fake\n"
        << "{\n"
        << "native int add(int a, int b);\n"
        << "}\n"
        << "native string greet(string who);\n";

    SymbolIndex index;
    index.LoadFile((tmp / "mylib.n").string());
    CHECK(index.HasPackage("mylib"));
    CHECK(!index.HasPackage("fake"));
    CHECK(index.Resolve("mylib", "add") == nullptr);
    const SymbolInfo* greet = index.Resolve("mylib", "greet");
    CHECK(greet != nullptr && greet->returnKind == TypeKind::String);
    fs::remove_all(tmp);
}

static void TestClassBodyMethodsSkipped() {
    // A class body at file top level is a scope the scanner must skip:
    // a method is not a package function. Project sources flow through
    // this scanner for go-to-definition, and user files carry classes.
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_classbody";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "shapes.n")
        << "class Point {\n"
        << "    int x;\n"
        << "    int getX() {\n"
        << "        return x;\n"
        << "    }\n"
        << "}\n"
        << "int area(Point p) {\n"
        << "    return 0;\n"
        << "}\n";

    SymbolIndex index;
    index.LoadLibraryDir(tmp.string());
    CHECK(index.Resolve("shapes", "area") != nullptr);
    CHECK(index.Resolve("shapes", "getX") == nullptr);
    fs::remove_all(tmp);
}

static void TestFindStdLibDir() {
    // Build <tmp>/a/b with <tmp>/a/stdlib; starting from b, the walk up must
    // find it.
    fs::path root = fs::temp_directory_path() / "nlang_ls_locate";
    fs::remove_all(root);
    fs::path start = root / "a" / "b";
    fs::create_directories(start / ".." / "stdlib");
    std::string found = FindStdLibDir(start.string());
    CHECK(!found.empty());
    CHECK(fs::exists(fs::path(found)));
    fs::remove_all(root);

    // No stdlib anywhere -> empty.
    fs::path none = fs::temp_directory_path() / "nlang_ls_none";
    fs::remove_all(none);
    fs::create_directories(none);
    CHECK(FindStdLibDir(none.string()).empty());
    fs::remove_all(none);
}

static void TestTypeKinds() {
    CHECK(TypeKindFromName("int") == TypeKind::Int);
    CHECK(TypeKindFromName("long") == TypeKind::Long);
    CHECK(TypeKindFromName("float") == TypeKind::Float);
    CHECK(TypeKindFromName("double") == TypeKind::Double);
    CHECK(TypeKindFromName("string") == TypeKind::String);
    CHECK(TypeKindFromName("List<string>") == TypeKind::ListString);
    //"any" is no longer a type kind: print takes a string, so an unknown
    //spelling falls through to Unknown like any other unrecognized name.
    CHECK(TypeKindFromName("any") == TypeKind::Unknown);
    CHECK(TypeKindFromName("void") == TypeKind::Void);
    CHECK(TypeKindFromName("widget") == TypeKind::Unknown);
    CHECK(NameOfTypeKind(TypeKind::Int) == "int");
    CHECK(NameOfTypeKind(TypeKind::Long) == "long");
    CHECK(NameOfTypeKind(TypeKind::Double) == "double");
    CHECK(NameOfTypeKind(TypeKind::ListString) == "List<string>");
    CHECK(NameOfTypeKind(TypeKind::Void) == "void");

    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);

    const SymbolInfo* print = index.Resolve("io", "print");
    CHECK(print != nullptr);
    if (print) {
        CHECK(print->returnKind == TypeKind::Void);
        CHECK(print->params.size() == 1
              && print->params[0].kind == TypeKind::String);
    }

    const SymbolInfo* sqrt = index.Resolve("math", "sqrt");
    CHECK(sqrt != nullptr);
    if (sqrt) {
        CHECK(sqrt->returnKind == TypeKind::Double);
        CHECK(sqrt->params.size() == 1
              && sqrt->params[0].kind == TypeKind::Double);
    }

    const SymbolInfo* absi = index.Resolve("math", "absi");
    CHECK(absi != nullptr && absi->params[0].kind == TypeKind::Int);

    const SymbolInfo* listFiles = index.Resolve("fs", "listFiles");
    CHECK(listFiles != nullptr
          && listFiles->returnKind == TypeKind::ListString);

    const SymbolInfo* writeFile = index.Resolve("io", "writeFile");
    CHECK(writeFile != nullptr && writeFile->returnKind == TypeKind::Void);
}

static void TestClear() {
    SymbolIndex index;
    index.LoadLibraryDir(STDLIB_DIR);
    CHECK(index.size() == 48);
    index.Clear();
    CHECK(index.size() == 0);
    CHECK(index.Resolve("io", "print") == nullptr);
    CHECK(index.Packages().empty());
    //Loaded-file markers were dropped too: the same dir re-indexes fully.
    index.LoadLibraryDir(STDLIB_DIR);
    CHECK(index.size() == 48);
}

static void TestBlockCommentBraces() {
    // Braces inside /* */ comments must not move the body depth, and
    // the block-comment state carries across lines (a spanning comment
    // governs every line until its closing '*/'). Both directions
    // matter at the top level: a '{' in comment text must not swallow
    // the next declaration, and a spanning comment's braces must not
    // leak onto the lines after it.
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_blockcmt";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "place.n")
        << "/* TODO: rework the { */\n"
        << "int place(int x) {\n"
        << "    /* a comment spanning\n"
        << "       lines with a } inside */\n"
        << "    return x;\n"
        << "}\n"
        << "/* another spanning one\n"
        << "   holding a { before the close */\n"
        << "int late() {\n"
        << "    return 2;\n"
        << "}\n";

    SymbolIndex index;
    index.LoadLibraryDir(tmp.string());
    CHECK(index.size() == 2);
    CHECK(index.Resolve("place", "place") != nullptr);
    CHECK(index.Resolve("place", "late") != nullptr);
    fs::remove_all(tmp);
}

static void TestLoadLibraryDirOnce() {
    //LoadLibraryDirOnce parses each dir once per generation (Clear
    //resets the guard), keyed by DirKey: folded . / .. segments, one
    //separator form, ASCII lower case on Windows — two spellings of
    //the same dir must not double the symbols.
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_dironce";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "once.n") << "int one() {\n    return 1;\n}\n";

    SymbolIndex index;
    index.LoadLibraryDirOnce(tmp.string());
    std::string alt = tmp.string();
    std::replace(alt.begin(), alt.end(), '\\', '/');
#ifdef _WIN32
    std::transform(alt.begin(), alt.end(), alt.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
#endif
    index.LoadLibraryDirOnce(alt);
    CHECK(index.size() == 1);
    CHECK(index.Resolve("once", "one") != nullptr);

    //A fresh generation (Clear) re-parses: edited content shows up.
    index.Clear();
    std::ofstream(tmp / "once.n") << "int one() {\n    return 1;\n}\n"
                                  << "int two() {\n    return 2;\n}\n";
    index.LoadLibraryDirOnce(tmp.string());
    CHECK(index.size() == 2);
    fs::remove_all(tmp);
}

static void TestResolveFirstInsertWins() {
    // Resolve returns the FIRST indexed match — a scan, not a lookup
    // that dedups. The IDE feeds library dirs in the compiler's search
    // order (import paths before the project's own dir), so a package
    // declared in both layers must resolve to the copy the compiler
    // builds: the earlier-indexed one.
    fs::path dirA = fs::temp_directory_path() / "nlang_ls_first_a";
    fs::path dirB = fs::temp_directory_path() / "nlang_ls_first_b";
    fs::remove_all(dirA);
    fs::remove_all(dirB);
    fs::create_directories(dirA);
    fs::create_directories(dirB);
    std::ofstream(dirA / "dup.n") << "int f(int a) {\n    return a;\n}\n";
    std::ofstream(dirB / "dup.n")
        << "string f(string s) {\n    return s;\n}\n";

    SymbolIndex index;
    index.LoadLibraryDir(dirA.string());
    index.LoadLibraryDir(dirB.string());
    //Both copies stay indexed (completion lists each); Resolve picks
    //the first.
    CHECK(index.size() == 2);
    const SymbolInfo* f = index.Resolve("dup", "f");
    CHECK(f != nullptr);
    if (f)
        CHECK(f->returnType == "int");
    fs::remove_all(dirA);
    fs::remove_all(dirB);
}

static void TestResolveDottedPackage() {
    // Go-to-definition splits a qualified name at the LAST dot: the
    // whole dotted prefix is the package ("gfx.color.draw" resolves as
    // package "gfx.color", name "draw"). This pins the documented
    // LoadFileOnce(path, package) contract: the CALLER supplies the
    // dotted package, and under a matched root the bare file stem is
    // not the package. (No in-tree caller feeds a dotted package yet;
    // the compiler's LoadLibrarySource forwards the raw matched root
    // today.)
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_dotted";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "gfx");
    std::ofstream(tmp / "gfx" / "color.n")
        << "int draw() {\n    return 1;\n}\n";

    SymbolIndex index;
    index.LoadFileOnce((tmp / "gfx" / "color.n").string(), "gfx.color");
    CHECK(index.Resolve("gfx.color", "draw") != nullptr);
    CHECK(index.Resolve("color", "draw") == nullptr);
    CHECK(index.HasPackage("gfx.color"));
    fs::remove_all(tmp);
}

int main() {
    TestLoadsRealStdLib();
    TestResolvePrint();
    TestSemanticParamNames();
    TestCompletion();
    TestNlangFunctionIsNotNative();
    TestPackageComesFromFilePath();
    TestClassBodyMethodsSkipped();
    TestBlockCommentBraces();
    TestFindStdLibDir();
    TestTypeKinds();
    TestClear();
    TestLoadLibraryDirOnce();
    TestResolveFirstInsertWins();
    TestResolveDottedPackage();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
