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
constexpr std::size_t kStdlibSymbolPin = 492;

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

    // 324 functions: io 19 (14 natives + 5 typed-reader wrappers; the
    // v11 clock/park natives and the byte helpers moved from net),
    // math 25, fs 10 (the binary read pair readBytes/readFloats),
    // thread 7 (the K3 materialized package plus the P2-2 failure
    // probes), tensor 209 (the v0.12.7 surface: broadcasting
    // elementwise with scalar arms, the view family
    // transpose(+axes)/flatten/slice/broadcastTo, axis sums, generic
    // reshape with -1 inference, elementwise exp/log/clamp, concat,
    // matmul, and the plain-NLang layer relu/softmax/crossEntropyLoss;
    // the v0.13.7 vector-ops layer: neg, the reversed scalar arms,
    // axis max/min, gather, argmax/argmin, eq, oneHot, uniform, and
    // the activation additions reluGrad/tanh; the v0.14.0 device line:
    // toDevice/toHost overloads for the four numeric kinds and
    // backend()),
    // net 54 (the script ping plus the internal _nowMs/_lastErrorText/_lastErrorCode/
    // _throwNet/_ping/resolveList, the _tcp* natives incl. the option
    // setters and the internal _toBytes/_fromBytes, the 20 _udp*
    // natives and the 5 _icmp* natives; class methods are not free
    // functions), system 2 (the wall and monotonic clocks), url 4 (the
    // percent and form codecs; the Url class members are not free
    // functions), gzip 4 (the compress/decompress wrappers and the two
    // native codec declarations), http 6 (the buffered convenience calls
    // get/post/put/del/request plus the lazy defaultClient; the Http*
    // class members are not free functions), regex 1 (the quoteMeta
    // escaping helper; the Regex/Match classes and the engine classes
    // are not free functions).
    // The index counts declaration lines; each symbol's package comes
    // from the file's path (the stem), not from any in-file head.
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
