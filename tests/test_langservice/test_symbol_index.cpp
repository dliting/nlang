/*--- test_symbol_index.cpp - SymbolIndex / FindStdLibDir unit tests -----*
 * Loads the real stdlib/*.n tree (no mocks) and pins the indexed shape.
 */
#include "nlang/langservice/SymbolIndex.h"

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
    // 38 functions: io 5, math 25, fs 8.
    CHECK(index.size() == 38);
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
        CHECK(s->params[0].type == "any");
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

    const auto fsSyms = index.CompleteNamespace("fs");
    CHECK(fsSyms.size() == 8);

    const auto namespaces = index.Namespaces();
    CHECK(namespaces.size() == 3);
    CHECK(namespaces[0] == "fs");
    CHECK(namespaces[1] == "io");
    CHECK(namespaces[2] == "math");

    CHECK(index.Resolve("io", "noSuchFn") == nullptr);
    CHECK(index.Resolve("nope", "print") == nullptr);
}

static void TestNlangFunctionIsNotNative() {
    // A function declared without 'native' must index as native=false.
    fs::path tmp = fs::temp_directory_path() / "nlang_ls_nonnative";
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    std::ofstream(tmp / "mylib.n")
        << "namespace mylib {\n"
        << "    // Add two ints.\n"
        << "    int add(int a, int b) {\n"
        << "        return a + b;\n"
        << "    }\n"
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

int main() {
    TestLoadsRealStdLib();
    TestResolvePrint();
    TestSemanticParamNames();
    TestCompletion();
    TestNlangFunctionIsNotNative();
    TestFindStdLibDir();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
