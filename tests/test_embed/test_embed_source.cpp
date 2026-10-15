//Source-entry embedding tests (spec 2026-10-15-embedding-source-entry):
//runSource/runFile compile+execute ad-hoc scripts in-process, through
//the public header, with real compilation and real execution (no mocks).
#include "nlang/embed/NLang.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace nlang;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
#define TEST(name) do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define CHECK_THROWS(excType, stmt, msg) \
    do { bool threw = false; \
         try { stmt; } catch (const excType&) { threw = true; } \
         if (!threw) { FAIL(msg); return; } } while(0)

#ifndef EMBED_DEPLOY_DIR
#error "EMBED_DEPLOY_DIR must be defined by the build"
#endif

//One captured output pair (out/err channels stay separate through
//setOutputHandler).
struct OutputSink {
    std::string out, err;
};
static void InstallSink(Interpreter& itp, OutputSink& sink) {
    itp.setOutputHandler(
        [&sink](const char* text) { sink.out += text; },
        [&sink](const char* text) { sink.err += text; });
}

#ifndef EMBED_SOURCE_COMPILER_UNIT
#define EMBED_SOURCE_COMPILER_UNIT
#include "SourceCompiler.h"

//Direct unit tests for the compile step (src/embed internal header,
//PRIVATE include -- same reach-through as test_embed/Marshalling.h).
static void TestSourceCompilerProducesArtifact() {
    TEST(TestSourceCompilerProducesArtifact);
    SourceCompiler::Input in;
    in.utf8Source = "return 0;";
    in.moduleName = "script";
    fs::path tempRoot;
    {
        SourceCompiler sc;
        const SourceCompiler::Output out = sc.Compile(in, {});
        CHECK(fs::exists(out.artifact), "entry .ncu written to temp dir");
        tempRoot = out.artifact.parent_path();
    }
    CHECK(!fs::exists(tempRoot), "temp dir removed after destruction");
    PASS();
}

static void TestSourceCompilerCollectsDiagnostics() {
    TEST(TestSourceCompilerCollectsDiagnostics);
    SourceCompiler::Input in;
    in.utf8Source = "int x = ;";
    in.moduleName = "script";
    SourceCompiler sc;
    bool threw = false;
    try {
        (void)sc.Compile(in, {});
    } catch (const LoadError& e) {
        threw = true;
        const std::string text = e.what();
        CHECK(text.find("Compilation failed.") != std::string::npos,
            "trailing failure marker");
        CHECK(text.find("script.n") != std::string::npos,
            "synthesized file name in diagnostics");
    }
    CHECK(threw, "compile failure throws LoadError");
    PASS();
}
#endif

int main() {
    initialize();
    TestSourceCompilerProducesArtifact();
    TestSourceCompilerCollectsDiagnostics();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
