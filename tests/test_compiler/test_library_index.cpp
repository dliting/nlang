/*---
test_library_index.cpp - BuildEnvironment library-index unit tests.

Verifies that a BuildEnvironment constructed with a stdlib directory loads
the declaration index (the future authority for function signatures) and
that an empty directory leaves the index empty instead of crashing.
Console-style suite (same shape as test_array_token).
---*/
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/langservice/SymbolIndex.h>
#include <cstdio>
#include <string>

using namespace nlang;

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

//Discards diagnostics; BuildEnvironment needs a logger but these tests do
//not compile anything, so no messages are expected.
class NullLogger : public CompileLogger
{
public:
    void WriteLog(CompileLogLevel, const ISourceLocation*,
        const char*) override {}
};

static void TestLoadsStdLib()
{
    BuildParams params;
    params.m_sStdLibDir = STDLIB_DIR;
    NullLogger logger;
    BuildEnvironment env(params, logger);

    // 48 functions: io 15 (10 natives + 5 typed-reader wrappers), math 25,
    // fs 8. The index counts declaration lines; each symbol's package
    // comes from the file's path (the stem), not from any in-file head.
    CHECK(env.LibraryIndex().size() == 48);

    //The "io" in this lookup now comes from the file's PATH (the stem of
    //io.n), not from any in-file head — same string, new source of truth.
    const langservice::SymbolInfo* print =
        env.LibraryIndex().Resolve("io", "print");
    CHECK(print != nullptr);
    if (print) {
        CHECK(print->native);
        CHECK(print->returnKind == langservice::TypeKind::Void);
        CHECK(print->params.size() == 1
              && print->params[0].kind == langservice::TypeKind::String);
    }

    const langservice::SymbolInfo* sqrt =
        env.LibraryIndex().Resolve("math", "sqrt");
    CHECK(sqrt != nullptr
          && sqrt->returnKind == langservice::TypeKind::Double);
}

static void TestEmptyDirLeavesIndexEmpty()
{
    BuildParams params;  // m_sStdLibDir left empty
    NullLogger logger;
    BuildEnvironment env(params, logger);
    CHECK(env.LibraryIndex().size() == 0);
    CHECK(env.LibraryIndex().Resolve("io", "print") == nullptr);
}

int main()
{
    TestLoadsStdLib();
    TestEmptyDirLeavesIndexEmpty();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
