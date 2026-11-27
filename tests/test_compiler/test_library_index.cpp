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

//Pin for the stdlib free-function surface (per-package breakdown in
//TestLoadsStdLib); the printer turns any future drift into a
//per-package diff instead of a bare FAIL line.
constexpr std::size_t kStdlibSymbolPin = 511;

static void PrintIndexShape(const langservice::SymbolIndex &index)
{
    std::printf("  index pin drift: size=%zu expected=%zu\n",
        index.size(), kStdlibSymbolPin);
    for (const std::string &pkg : index.Packages())
        std::printf("    %s: %zu\n", pkg.c_str(),
            index.CompletePackage(pkg).size());
}

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

    // 511 free functions across 13 packages: fs 12, gzip 4, http 6, io 22,
    // json 1, math 26, net 56, regex 1, system 2, tensor 369, thread 7,
    // url 4, web 1. The index counts declaration lines; each symbol's
    // package comes from the file's path (the stem), not from any
    // in-file head. Per-package free-function surface (class methods
    // are not free functions and do not count):
    //   io 22: the console/input natives, the typed readers, the file
    //     natives, the park native, the byte helpers and the base64 codecs.
    //   fs 12: the binary read pair readBytes/readFloats plus writeBytes
    //     and modifiedTimeMs.
    //   json 1: parse (the JSON entry point; the Json* classes are not
    //     free functions).
    //   math 26: the scalar/integer math surface plus randomBytes.
    //   net 56: the script ping, the internal natives and the _tcp*/_udp*/
    //     _icmp* native families.
    //   system 2: the wall and monotonic clocks.
    //   tensor 369: the branch's full tensor surface (broadcasting
    //     elementwise, the view family, axis reductions, generic reshape,
    //     elementwise exp/log/clamp, concat, matmul, the plain-NLang layer,
    //     the vector-ops layer, and the device line toDevice/toHost/
    //     backend()/deviceCount/streamCreate with device-spelled transfer).
    //   thread 7: the K3 materialized package plus the failure probes.
    //   url 4: the percent and form codecs.
    //   gzip 4: the compress/decompress wrappers and the two native codec
    //     declarations.
    //   http 6: the buffered convenience calls get/post/put/del/request
    //     plus the lazy defaultClient.
    //   regex 1: the quoteMeta escaping helper.
    //   web 1: hexEncode (the App/Route/Session classes are not free
    //     functions).
    if (env.LibraryIndex().size() != kStdlibSymbolPin)
        PrintIndexShape(env.LibraryIndex());
    CHECK(env.LibraryIndex().size() == kStdlibSymbolPin);

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
