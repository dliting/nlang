//Source-entry embedding tests (spec 2026-10-15-embedding-source-entry):
//runSource/runFile compile+execute ad-hoc scripts in-process, through
//the public header, with real compilation and real execution (no mocks).
#include "nlang/embed/NLang.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/ModuleBuilder.h>
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

//D9 contract: log items stay readable after the owning ModuleBuilder
//died -- ListCompileLogger's collect-then-read contract. The location
//clone must own the file name, not dangle a TU pointer into the
//destroyed builder.
static void TestDiagnosticsSurviveBuilderDestruction() {
    TEST(TestDiagnosticsSurviveBuilderDestruction);
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "nlang_e_diag_pin";
    CHECK(!ec, "temp directory locatable");
    fs::create_directories(dir, ec);
    const fs::path source = dir / "diag_pin.n";
    {
        std::ofstream out(source, std::ios::binary);
        out << "int x = ;";
    }
    BuildParams params;
    params.m_SourceFiles.push_back(source.string());
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = "diag_pin";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_ImportDirs.clear();
    TypeCastInfo::StaticInit();
    ListCompileLogger logger;
    {
        ModuleBuilder builder(params, logger);
        (void)builder.BuildArtifacts();
    }   //builder died: the TUs it owned are freed
    std::string text;
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        text += (*it)->FullMessage();
    CHECK(text.find("diag_pin.n") != std::string::npos,
        "file name in diagnostics survives builder destruction");
    fs::remove_all(dir, ec);
    PASS();
}
#endif

//--- Interpreter source entry (spec §6) --------------------------------
//§6.1（io.print，依赖 native 接线）在 Task 4 落地——本任务的用例全部
//不依赖 native 解析，build-dev 与全新树行为一致。

static void TestRunSourceTopLevelReturn() {           //§6.2
    TEST(TestRunSourceTopLevelReturn);
    Interpreter itp;
    const int rc = itp.runSource("return 30;");
    CHECK(rc == 30, "top-level return value is the exit code");
    PASS();
}

static void TestCompileFailureLoadError() {           //§6.6 + §6.12
    TEST(TestCompileFailureLoadError);
    Interpreter itp;
    bool threw = false;
    try {
        (void)itp.runSource("int x = ;");
    } catch (const LoadError& e) {
        threw = true;
        const std::string text = e.what();
        CHECK(text.find("script.n") != std::string::npos,
            "synthesized file name (spec 12)");
        CHECK(text.find("Compilation failed.") != std::string::npos,
            "trailing marker");
    }
    CHECK(threw, "compile failure throws LoadError");
    //失败后 call 不崩溃：无程序来源时按 v1 契约拒绝（spec §6.6）。
    CHECK_THROWS(BadValue, (void)itp.call("script.anything", {}),
        "call after compile failure -> BadValue, not a crash");
    //编译失败不消耗唯一来源：同一 Interpreter 可重试（spec §2）。
    CHECK(itp.runSource("return 1;") == 1, "retry after compile failure");
    PASS();
}

static void TestUncaughtThrowSurfacesException() {    //§6.7
    TEST(TestUncaughtThrowSurfacesException);
    Interpreter itp;
    bool threw = false;
    try {
        (void)itp.runSource("throw Exception(\"boom\");");
    } catch (const Exception& e) {
        threw = true;
        CHECK(std::string(e.message()).find("boom") != std::string::npos,
            "message carried");
    }
    CHECK(threw, "uncaught script throw surfaces as nlang::Exception");
    PASS();
}

static void TestEmptySourceThrowsLoadError() {        //§4 边界（钉死）
    TEST(TestEmptySourceThrowsLoadError);
    Interpreter itp;
    CHECK_THROWS(LoadError, (void)itp.runSource(""),
        "empty source has no entry point -> LoadError");
    PASS();
}

int main() {
    initialize();
    TestSourceCompilerProducesArtifact();
    TestSourceCompilerCollectsDiagnostics();
    TestDiagnosticsSurviveBuilderDestruction();
    TestRunSourceTopLevelReturn();
    TestCompileFailureLoadError();
    TestUncaughtThrowSurfacesException();
    TestEmptySourceThrowsLoadError();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
