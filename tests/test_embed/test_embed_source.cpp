//Source-entry embedding tests (spec 2026-10-15-embedding-source-entry):
//runSource/runFile compile+execute ad-hoc scripts in-process, through
//the public header, with real compilation and real execution (no mocks).
#include "nlang/embed/NLang.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <windows.h>   //GetCurrentProcessId（ScratchDir 的 PID 前缀）
#endif

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

//Both real-channel fallback tests read the captured file through one
//chunked loop; kReadChunk is the read granularity, not a protocol size.
constexpr size_t kReadChunk = 256;
static std::string ReadFileText(const fs::path& file) {
    std::string text;
    std::ifstream in(file, std::ios::binary);
    char buf[kReadChunk];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        text.append(buf, static_cast<size_t>(in.gcount()));
    return text;
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

static void WriteText(const fs::path& p, const char* text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
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
    WriteText(source, "int x = ;");
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

//Unique scratch dir for runFile tests (PID-then-counter suffix,
//RAII-cleaned): the PID prefix aligns with the product's temp-run-dir
//form (nlang_e_<pid>_) so a crashed run's leftover directory cannot be
//silently reused, and concurrent runs cannot collide.
static unsigned ProcessId() {
#ifdef _WIN32
    return static_cast<unsigned>(GetCurrentProcessId());
#else
    return static_cast<unsigned>(::getpid());
#endif
}
struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        path = fs::temp_directory_path(ec)
            / ("nlang_embed_src_t_" + std::to_string(ProcessId())
               + "_" + std::to_string(counter++));
        fs::create_directories(path, ec);
    }
    ~ScratchDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

//Redirects one stdio channel (fd 1 or 2) to a file for the scope and
//restores the original descriptor on every exit path (CHECK returns
//included — a leaked redirect would corrupt later diagnostics; quality
//review M-A). The destructor flushes BEFORE restoring so captured bytes
//reach the file even when the channel was fully buffered.
class FdCapture {
public:
    FdCapture(int fd, const fs::path& to)
        : m_fd(fd), m_stream(fd == 1 ? stdout : stderr) {
        m_savedFd = _dup(fd);
        m_taken = m_savedFd >= 0
            && std::freopen(to.string().c_str(), "w", m_stream) != nullptr;
    }
    ~FdCapture() {
        if (m_savedFd < 0)
            return;
        if (m_taken)
            std::fflush(m_stream);
        _dup2(m_savedFd, m_fd);
        _close(m_savedFd);
    }
    bool ok() const { return m_taken; }
    FdCapture(const FdCapture&) = delete;
    FdCapture& operator=(const FdCapture&) = delete;
private:
    int m_fd;
    std::FILE* m_stream;
    int m_savedFd = -1;
    bool m_taken = false;
};

//--- Interpreter source entry (spec §6) --------------------------------
//§6.1（io.print，依赖 native 接线）在 Task 4 落地——Task 3 的用例均
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

//§6.1 从 Task 3 移到这里：io.print 依赖 native 解析，接线在 Step 3
//才落——放在 Task 3 会在 D6 之前留下一个必然失败的用例。
static void TestRunSourceBoilerplateFree() {          //§6.1
    TEST(TestRunSourceBoilerplateFree);
    Interpreter itp;
    itp.addImportDir(EMBED_DEPLOY_DIR);
    OutputSink sink; InstallSink(itp, sink);
    const int rc = itp.runSource("import io; io.print(6*7);");
    CHECK(rc == 0, "no top-level return -> exit 0");
    CHECK(sink.out.find("42") != std::string::npos, "io.print output");
    PASS();
}

static void TestV1LoadPathNativeWiring() {            //§6.13（D6 回归）
    TEST(TestV1LoadPathNativeWiring);
    Interpreter itp;
    itp.addImportDir(EMBED_DEPLOY_DIR);
    itp.load(EMBED_IO_NCU);       //io_demo imports the stdlib io module
    OutputSink sink; InstallSink(itp, sink);
    CHECK(itp.run() == 0, "v1 load+run still works");
    //emitBoth uses io.print/io.eprint at runtime: closure (stdlib.npkg)
    //and native (nlang_io.dll) must both resolve from the search dirs,
    //not from beside the test exe.
    Value r = itp.call("io_demo.emitBoth", {});
    CHECK(r.kind() == Value::Kind::Int && r.asInt() == 0,
        "io-using call resolves stdlib+natives from import dirs");
    CHECK(sink.out.find("to-out") != std::string::npos, "out channel");
    CHECK(sink.err.find("to-err") != std::string::npos, "err channel");
    PASS();
}

//§6.13 补（Task 3 质量审查 I-1）：部分装设的空通道回退真实 stdio
//——「空＝该通道不重定向」承诺的就是默认 stdio 行为，而非丢弃。
static void TestPartialOutputHandlerFallsBackToStderr() {
    TEST(TestPartialOutputHandlerFallsBackToStderr);
    Interpreter itp;
    itp.addImportDir(EMBED_DEPLOY_DIR);
    OutputSink sink;   //只装 out：err 通道应走真实 stderr
    itp.setOutputHandler(
        [&sink](const char* text) { sink.out += text; }, nullptr);
    ScratchDir dir;
    const fs::path errFile = dir.path / "stderr.txt";
    {
        //fd 2 换到临时文件，捕获「真实 stderr」的字节
        FdCapture capture(2, errFile);
        CHECK(capture.ok(), "redirect stderr to file");
        const int rc = itp.runSource(
            "import io; io.print(\"to-out\"); io.eprint(\"to-err\");");
        CHECK(rc == 0, "script runs");
    }   //guard：先冲刷再恢复 fd 2
    CHECK(sink.out.find("to-out") != std::string::npos,
        "out channel delivered to host");
    CHECK(ReadFileText(errFile).find("to-err") != std::string::npos,
        "empty err channel falls back to real stderr");
    PASS();
}

//I-1 out 侧对偶（Task 4 质量审查）：空 out 通道走真实 stdout。作用域
//内先读一次——逐写冲刷（WriteRealChannel 的 fflush）是「重定向的
//全缓冲 stdout 下字节及时可见」的直接检验；恢复后再读补钉整链路。
static void TestPartialOutputHandlerFallsBackToStdout() {
    TEST(TestPartialOutputHandlerFallsBackToStdout);
    Interpreter itp;
    itp.addImportDir(EMBED_DEPLOY_DIR);
    OutputSink sink;   //只装 err：out 通道应走真实 stdout
    itp.setOutputHandler(
        nullptr, [&sink](const char* text) { sink.err += text; });
    ScratchDir dir;
    const fs::path outFile = dir.path / "stdout.txt";
    {
        FdCapture capture(1, outFile);
        CHECK(capture.ok(), "redirect stdout to file");
        const int rc = itp.runSource(
            "import io; io.print(\"to-out\"); io.eprint(\"to-err\");");
        CHECK(rc == 0, "script runs");
        CHECK(ReadFileText(outFile).find("to-out") != std::string::npos,
            "per-write flush lands bytes before channel restore");
    }
    CHECK(sink.err.find("to-err") != std::string::npos,
        "err channel delivered to host");
    CHECK(ReadFileText(outFile).find("to-out") != std::string::npos,
        "empty out channel falls back to real stdout");
    PASS();
}

static void TestRunFileRealPathNames() {              //§6.3
    TEST(TestRunFileRealPathNames);
    ScratchDir dir;
    const fs::path demo = dir.path / "demo.n";
    WriteText(demo, "import io; io.print(11); return 5;");
    Interpreter itp;
    itp.addImportDir(EMBED_DEPLOY_DIR);
    OutputSink sink; InstallSink(itp, sink);
    CHECK(itp.runFile(demo) == 5, "runFile returns top-level return");
    CHECK(sink.out.find("11") != std::string::npos, "script output");
    //诊断带真实文件名：坏文件 → LoadError 文本含 demo.n＋其所在目录
    //（仅 stem 断言会被「同 stem 拷进 temp 编译」的形态假绿，质量审查 M5）。
    const fs::path bad = dir.path / "demo_bad.n";
    WriteText(bad, "int x = ;");
    Interpreter itp2;
    bool threw = false;
    try {
        (void)itp2.runFile(bad);
    } catch (const LoadError& e) {
        threw = true;
        const std::string text = e.what();
        CHECK(text.find("demo_bad.n") != std::string::npos,
            "real file name in diagnostics");
        CHECK(text.find(dir.path.string()) != std::string::npos,
            "real directory in diagnostics");
    }
    CHECK(threw, "compile failure in file form throws LoadError");
    PASS();
}

static void TestRunFileLeavesNoArtifacts() {          //§6.11
    TEST(TestRunFileLeavesNoArtifacts);
    ScratchDir dir;
    const fs::path demo = dir.path / "solo.n";
    WriteText(demo, "return 0;");
    Interpreter itp;
    CHECK(itp.runFile(demo) == 0, "runFile returns 0");
    std::error_code ec;
    const size_t entries =
        std::distance(fs::directory_iterator(dir.path, ec),
                      fs::directory_iterator());
    CHECK(entries == 1, "script dir holds only the .n source");
    PASS();
}

static void TestRunFileBadStemRejected() {            //§3 步骤 1
    TEST(TestRunFileBadStemRejected);
    ScratchDir dir;
    const fs::path bad = dir.path / "my-tool.n";   //dash: invalid stem
    WriteText(bad, "return 0;");
    Interpreter itp;
    bool threw = false;
    try {
        (void)itp.runFile(bad);
    } catch (const LoadError& e) {
        threw = true;
        const std::string text = e.what();
        //明确文案来自 SourceCompiler 入口的 Module::IsValidName
        //预校验（D8 审查 Low 2：Create 的 LogError 是裸 stderr、
        //不进诊断收集，靠它则错误无理由文本）。
        CHECK(text.find("invalid module name") != std::string::npos,
            "explicit invalid-module-name message");
        CHECK(text.find("my-tool") != std::string::npos,
            "offending stem named in the message");
    }
    CHECK(threw, "non-identifier stem -> LoadError");
    PASS();
}

//§3 生效搜索表（质量审查 M3）：文件形态把脚本所在目录并入搜索
//表。helper 只有源码、无预编译包——按装载架构（库代码走预编译
//包，源码只供签名）装载报 not found，但「import 的编译期签名解析
//成功＋装载失败信息把脚本目录列为首个搜索目录」共同钉住并入生
//效：删并入逻辑则编译期即报 import 解析失败，首断言实测转红（次
//断言作冗余面，未单独突变验证）（测试形态偏离计划原案——原案断
//言 rc==42，经 ncc/nvm 探针证实不可达：helper.n 源码不足以供装载，
//工具链同报 not found）。
static void TestRunFileScriptDirOnSearchPath() {
    TEST(TestRunFileScriptDirOnSearchPath);
    ScratchDir dir;
    WriteText(dir.path / "helper.n",
        "int twice(int v) { return v*2; }");
    WriteText(dir.path / "main_like.n",
        "import helper; return twice(21);");
    Interpreter itp;   //不 addImportDir——脚本目录是唯一搜索来源
    bool threw = false;
    try {
        (void)itp.runFile(dir.path / "main_like.n");
    } catch (const LoadError& e) {
        threw = true;
        const std::string text = e.what();
        CHECK(text.find("module 'helper' not found") != std::string::npos,
            "helper resolves signatures but has no prebuilt package");
        CHECK(text.find(dir.path.string()) != std::string::npos,
            "script's own directory is on the load search list");
    }
    CHECK(threw, "runFile reports the load failure");
    PASS();
}

static void TestCallAfterRunSource() {                //§6.4
    TEST(TestCallAfterRunSource);
    Interpreter itp;
    (void)itp.runSource("int add(int a, int b) { return a+b; } return 0;");
    Value r = itp.call("script.add",
                       {Value(int32_t(2)), Value(int32_t(3))});
    CHECK(r.kind() == Value::Kind::Int && r.asInt() == 5,
        "call(\"script.add\") after runSource (package-qualified key)");
    PASS();
}

static void TestSourceExclusivity() {                 //§6.5
    TEST(TestSourceExclusivity);
    Interpreter itp;
    (void)itp.runSource("return 0;");
    CHECK_THROWS(BadValue, itp.runSource("return 0;"),
        "second runSource -> BadValue");
    CHECK_THROWS(BadValue, itp.load(EMBED_IO_NCU),
        "load() after runSource -> BadValue");
    //runFile 的来源必须是 .n 源码——拿现成 .ncu 顶替会把「stem 校验
    //拒绝」与「互斥拒绝」混进同一断言；临时写一个真源文件分开钉。
    ScratchDir dir;
    const fs::path soloScript = dir.path / "second.n";
    WriteText(soloScript, "return 0;");
    CHECK_THROWS(BadValue, itp.runFile(soloScript),
        "runFile after runSource -> BadValue");
    CHECK_THROWS(BadValue, itp.addImportDir("."),
        "addImportDir after runSource -> BadValue");
    PASS();
}

//§6.6 延伸（Task 3 质量审查 M-2 行为覆盖）：装载失败（编译成功但
//无入口）同样不消耗唯一来源——换 runSource 立即可重试；同时跑过
//Task 4 Step 6 的脚本目录去重路径（重试后搜索表不重复累计）。
//纯函数定义不触发合成 main（v0.9.1：顶层**语句**段落才包裹；对照
//e2e script_decls_only）——整文件只有声明即无入口。
static void TestRetryAfterLoadFailure() {               //§6.6 延伸
    TEST(TestRetryAfterLoadFailure);
    Interpreter itp;
    ScratchDir dir;
    const fs::path noEntry = dir.path / "no_entry.n";
    WriteText(noEntry, "int f() { return 0; }");
    CHECK_THROWS(LoadError, (void)itp.runFile(noEntry),
        "declarations-only script has no entry -> LoadError");
    CHECK(itp.runSource("return 1;") == 1,
        "source still available after load failure");
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
    TestRunSourceBoilerplateFree();
    TestV1LoadPathNativeWiring();
    TestPartialOutputHandlerFallsBackToStderr();
    TestPartialOutputHandlerFallsBackToStdout();
    TestRunFileRealPathNames();
    TestRunFileLeavesNoArtifacts();
    TestRunFileBadStemRejected();
    TestRunFileScriptDirOnSearchPath();
    TestCallAfterRunSource();
    TestSourceExclusivity();
    TestRetryAfterLoadFailure();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
