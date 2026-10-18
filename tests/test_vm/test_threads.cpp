/*---
    test_threads.cpp - 并发 K1（帧栈机 + 虚拟线程/协程）单元测试。

    Harness 形态与 test_primops Layer 3 相同：真实 .n 源码经
    ModuleBuilder 编译、NcuLoader/NcuLinker 装链接、VmExecutor 真实执行，
    断言 main() 返回值。本文件随切片增长：
      Slice 0  冒烟（本文件）；
      后续切片 加入调度/挂起/根集等行为测试与测试专用旋钮。
    确定性规约（计划钉死）：断言只用顺序/终值/happens-before，不做墙钟
    断言；交替顺序仅经测试专用配额旋钮强制。
---*/
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_threads";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + execute; returns main()'s value, or -1 when the build failed
//(same harness shape as test_primops — real sources, real execution).
static int runSource(const std::string& tag, const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    try {
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        std::cerr << "build failed: " << e.what() << "\n";
        return -1;
    }
    if (!built || !std::filesystem::exists(modPath)) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << (*it)->Message() << "\n";
        return -1;
    }
    //Load/link failures and run-time exceptions (recursion limit, uncaught
    //NLang throws) propagate to the caller — pinning tests assert on both
    //the throw and its message.
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    CompiledModule mod = NcuLinker::Link(std::move(loaded.units),
                                         loaded.entryKey);
    VmExecutor exec;
    return exec.Execute(mod);
}

void test_harness_smoke()
{
    TEST(harness_smoke);
    const int rc = runSource("smoke",
        "int main() {\n"
        "    return 7;\n"
        "}\n");
    CHECK(rc == 7, "compile + execute round trip returns main value");
    PASS();
}

void test_harness_smoke_exception_path()
{
    TEST(harness_smoke_exception_path);
    const int rc = runSource("smoke_exc",
        "int main() {\n"
        "    try {\n"
        "        throw Exception(\"boom\");\n"
        "    } catch (Exception e) {\n"
        "        if (e.message == \"boom\") return 0;\n"
        "        return 2;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "NLangThrow catch path works through the harness");
    PASS();
}

// --- Slice 1 钉死测试（帧栈化前后行为等价预言）---

//递归上限：报错文本与传播路径（std::runtime_error 穿出 Execute）不变。
void test_frame_recursion_limit_message()
{
    TEST(frame_recursion_limit_message);
    bool threw = false;
    std::string what;
    try {
        runSource("frame_rec_limit",
            "int f(int n) { return f(n + 1); }\n"
            "int main() { return f(0); }\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "deep recursion throws");
    CHECK(what.find("recursion limit exceeded") != std::string::npos,
          "message names the recursion limit");
    PASS();
}

//限内深递归：200 层压帧/弹帧往返正确。
void test_frame_deep_recursion_succeeds()
{
    TEST(frame_deep_recursion_succeeds);
    const int rc = runSource("frame_rec_sum",
        "int sum(int n) {\n"
        "    if (n <= 0) return 0;\n"
        "    return n + sum(n - 1);\n"
        "}\n"
        "int main() { return sum(200); }\n");
    CHECK(rc == 200 * 201 / 2, "200-deep recursion returns the exact sum");
    PASS();
}

//递归边界逐位对齐（等价预言）：递归引擎以 m_recurseDepth 计数，全部
//ExecuteFunction 调用（含 main 根）共用 RECURSE_LIMIT=1000 — 前 1000 个
//帧成功、第 1001 个帧抛。帧机以 m_callStack.size() 计数，边界必须重合：
//main 根 + 999 次 sum 调用（n∈[0,998]）= 1000 帧成功；sum(999) 需要
//第 1001 帧，抛 "recursion limit exceeded"。
void test_frame_recursion_boundary_exact()
{
    TEST(frame_recursion_boundary_exact);
    //sum(998): 999 sum calls + main root = 1000 frames: succeeds.
    const int okRc = runSource("frame_rec_b999",
        "int sum(int n) {\n"
        "    if (n <= 0) return 0;\n"
        "    return n + sum(n - 1);\n"
        "}\n"
        "int main() { return sum(998); }\n");
    CHECK(okRc == 998 * 999 / 2, "999 calls + main = 1000 frames, succeeds");
    //sum(999): n∈[0,999] = 1000 calls + main = 1001st frame: throws.
    bool threw = false;
    std::string what;
    try {
        runSource("frame_rec_b1000",
            "int sum(int n) {\n"
            "    if (n <= 0) return 0;\n"
            "    return n + sum(n - 1);\n"
            "}\n"
            "int main() { return sum(999); }\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "1001st frame throws");
    CHECK(what.find("recursion limit exceeded") != std::string::npos,
          "boundary throw names the recursion limit");
    PASS();
}

//out 写回四 ABI：自由函数（OP_CallFuncOut）。
void test_frame_out_writeback_func()
{
    TEST(frame_out_writeback_func);
    const int rc = runSource("frame_out_func",
        "void grab(out int r) { r = 7; }\n"
        "int main() {\n"
        "    int box = 0;\n"
        "    grab(out box);\n"
        "    return box;\n"
        "}\n");
    CHECK(rc == 7, "free-function out param writes back to the caller slot");
    PASS();
}

//out 写回四 ABI：直接方法调用（OP_CallMethodDirectOut，this 占 slot 0）。
void test_frame_out_writeback_method_direct()
{
    TEST(frame_out_writeback_method_direct);
    const int rc = runSource("frame_out_mdirect",
        "class Box {\n"
        "    int v;\n"
        "    public void fill(out int r) { r = v + 5; }\n"
        "}\n"
        "int main() {\n"
        "    Box b = Box();\n"
        "    b.v = 40;\n"
        "    int r = 0;\n"
        "    b.fill(out r);\n"
        "    return r;\n"
        "}\n");
    CHECK(rc == 45, "direct-method out param writes back past `this` slot");
    PASS();
}

//out 写回四 ABI：自由委托（CallDelegateFree，OP_CallDelegateOut）。
void test_frame_out_writeback_delegate_free()
{
    TEST(frame_out_writeback_delegate_free);
    const int rc = runSource("frame_out_deleg_free",
        "void grab(out int r) { r = 7; }\n"
        "int main() {\n"
        "    func<void, out int> f = grab;\n"
        "    int box = 0;\n"
        "    f(out box);\n"
        "    return box;\n"
        "}\n");
    CHECK(rc == 7, "free-delegate out param writes back");
    PASS();
}

//out 写回四 ABI：绑定方法委托（CallDelegateBound——接收者占 slot 0，
//用户参数右移一格，写回须逆向移位）。
void test_frame_out_writeback_delegate_bound()
{
    TEST(frame_out_writeback_delegate_bound);
    const int rc = runSource("frame_out_deleg_bound",
        "class Box {\n"
        "    int v;\n"
        "    public void fill(out int r) { r = v + 5; }\n"
        "}\n"
        "int main() {\n"
        "    Box b = Box();\n"
        "    b.v = 40;\n"
        "    func<void, out int> f = b.fill;\n"
        "    int r = 0;\n"
        "    f(out r);\n"
        "    return r;\n"
        "}\n");
    CHECK(rc == 45, "bound-delegate out write-back reverses the this shift");
    PASS();
}

//跨帧异常：深层抛出、中层捕获，执行继续且后续调用帧正常。
void test_frame_throw_across_frames_catch_continues()
{
    TEST(frame_throw_across_frames_catch_continues);
    const int rc = runSource("frame_throw_across",
        "int boom(int n) {\n"
        "    if (n == 0) throw Exception(\"deep\");\n"
        "    return boom(n - 1);\n"
        "}\n"
        "int after() { return 3; }\n"
        "int main() {\n"
        "    try {\n"
        "        boom(5);\n"
        "    } catch (Exception e) {\n"
        "        if (e.message != \"deep\") return 9;\n"
        "    }\n"
        "    return after();\n"
        "}\n");
    CHECK(rc == 3, "catch across popped frames then a fresh call works");
    PASS();
}

int main()
{
#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    //Hidden host contract (same as ncc/nvm main): the compiler needs the
    //IdString tables and the cast table (module boundary: runtime stays
    //compiler-free).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== NLang Threads Unit Tests ===\n\n";

    try {
        test_harness_smoke();
        test_harness_smoke_exception_path();
        test_frame_recursion_limit_message();
        test_frame_deep_recursion_succeeds();
        test_frame_recursion_boundary_exact();
        test_frame_out_writeback_func();
        test_frame_out_writeback_method_direct();
        test_frame_out_writeback_delegate_free();
        test_frame_out_writeback_delegate_bound();
        test_frame_throw_across_frames_catch_continues();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
