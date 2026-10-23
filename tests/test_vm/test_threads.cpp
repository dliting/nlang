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
#include "nlang/vm/NativeHost.h"
#include "VmExecutor.h"
#include "IHostIo.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

//Compile one source file to its .ncu on disk; the load/link tail is the
//caller's. Throws std::runtime_error with diagnostics on build failure —
//a failure here is a test bug, not behavior under test.
static std::filesystem::path compileUnitToDisk(
    const std::string& tag, const std::string& source,
    const std::list<std::string>& importDirs = {})
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
    params.m_ImportDirs = importDirs;
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
        throw std::runtime_error(std::string("build failed: ") + e.what());
    }
    if (!built || !std::filesystem::exists(modPath)) {
        std::string diag;
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            diag += (*it)->Message() + "\n";
        throw std::runtime_error("build failed:\n" + diag);
    }
    return modPath;
}

//Compile + link; the caller drives its own VmExecutor (scheduler knobs
//must be set before Execute). Throws std::runtime_error with diagnostics
//on build failure — a failure here is a test bug, not behavior under
//test.
static CompiledModule buildLinkedModule(const std::string& tag,
                                        const std::string& source)
{
    const auto modPath = compileUnitToDisk(tag, source);
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {scratchDir().string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    return NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
}

//Compile + execute; returns main()'s value, or -1 when the build failed
//(same harness shape as test_primops — real sources, real execution).
static int runSource(const std::string& tag, const std::string& source)
{
    //Load/link failures and run-time exceptions (recursion limit, uncaught
    //NLang throws) propagate to the caller — pinning tests assert on both
    //the throw and its message.
    CompiledModule mod = buildLinkedModule(tag, source);
    VmExecutor exec;
    return exec.Execute(mod);
}

//Slice 6 observation channel: spawned workers cannot return values in K1,
//so tests capture program output through the host I/O seam (deterministic
//order assertions — the plan's no-wall-clock rule). io.write/io.print
//arrive here verbatim via OnOutput (write = no appended newline, the
//form exact-string assertions want).
class CaptureIo : public IHostIo {
public:
    void OnOutput(std::string_view text) override { m_out += text; }
    std::string m_out;
};

//runSource + output capture. Run-time exceptions still propagate.
static int runSourceCapturing(const std::string& tag,
                              const std::string& source, std::string& rOut)
{
    CompiledModule mod = buildLinkedModule(tag, source);
    VmExecutor exec;
    CaptureIo io;
    exec.SetHostIo(&io);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
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
//帧成功、第 1001 个帧抛。帧机以当前求值栈的帧数计数，边界必须重合：
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
        "    public int v;\n"
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
        "    public int v;\n"
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

// --- Slice 4 钉死测试（调度器骨架：配额不抢占单上下文 + 注册表生命周期）---

//决策 5 的核心不变量：配额到期仅是「允许调度」，不是「必须调度」——
//没有其他 Runnable 上下文时预算原样重装、执行继续，单上下文程序逐位
//不变。注入微小配额（2）强制 500 次回边里产生数百次到期，断言每次
//到期都走了「跳过」分支（skip 计数与 fire 计数全等），程序结果精确。
void test_scheduler_quota_skips_single_context()
{
    TEST(scheduler_quota_skips_single_context);
    CompiledModule mod = buildLinkedModule("sched_quota_alone",
        "int main() {\n"
        "    int sum = 0;\n"
        "    int i = 0;\n"
        "    while (i < 500) {\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (sum != 124750) return 1;\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetSchedulerTripForTests(2);
    const int rc = exec.Execute(mod);
    CHECK(rc == 0,
          "single-context program completes correctly under a tiny quota");
    CHECK(exec.SchedulerQuotaFires() > 0,
          "the injected quota is genuinely exhausted at back-edges");
    CHECK(exec.SchedulerPreemptSkips() == exec.SchedulerQuotaFires(),
          "every quota fire is skipped: no other runnable context exists");
    PASS();
}

//默认配额（DEFAULT_SCHEDULER_TRIP_BACKEDGES = 1000）长循环可到期，但
//单上下文下同样只走跳过分支——用测试固定常量在真实执行路径上生效。
void test_scheduler_default_quota_single_context()
{
    TEST(scheduler_default_quota_single_context);
    CompiledModule mod = buildLinkedModule("sched_quota_default",
        "int main() {\n"
        "    int sum = 0;\n"
        "    int i = 0;\n"
        "    while (i < 5000) {\n"
        "        sum = sum + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (sum != 12497500) return 1;\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    const int rc = exec.Execute(mod);
    CHECK(rc == 0, "long single-context loop completes correctly");
    CHECK(exec.SchedulerQuotaFires() > 0,
          "the default 1000-trip budget is reached by a 5000-iteration loop");
    CHECK(exec.SchedulerPreemptSkips() == exec.SchedulerQuotaFires(),
          "default quota also never preempts a lone context");
    PASS();
}

//注册表生命周期（决策 5 显式枚举清空，嵌入宿主复用执行器的前提）：
//新执行器零上下文；一次运行恰注册 main（id 0）并迁移到 Finished 且
//根帧返回值入库；同执行器二次运行后注册表仍是恰一个上下文——上一轮
//的注册表/运行队列/定时器表被复位，无泄漏累积。
void test_scheduler_registry_lifecycle()
{
    TEST(scheduler_registry_lifecycle);
    CompiledModule mod = buildLinkedModule("sched_lifecycle",
        "int main() { return 3; }\n");
    VmExecutor exec;
    CHECK(exec.SchedulerContextCount() == 0,
          "a fresh executor owns no contexts");
    CHECK(exec.Execute(mod) == 3, "first run executes");
    CHECK(exec.SchedulerContextCount() == 1,
          "exactly the main context is registered");
    const ExecutionContext* mainCtx = exec.SchedulerContext(0);
    CHECK(mainCtx != nullptr, "main context is findable by id");
    CHECK(mainCtx->state == ContextState::Finished,
          "main reaches Finished after a normal return");
    CHECK(mainCtx->resultCell == 3,
          "the root-frame return value is stored on the context");
    CHECK(exec.Execute(mod) == 3,
          "second run on the same executor (embedding-host reuse shape)");
    CHECK(exec.SchedulerContextCount() == 1,
          "per-run reset keeps the registry at exactly one context");
    CHECK(exec.SchedulerContext(0) != nullptr
          && exec.SchedulerContext(0)->state == ContextState::Finished,
          "the re-run context is again main and Finished");
    //失败路径：根帧异常逃逸走 catch(...) 分支 → MarkFailed。
    CompiledModule boom = buildLinkedModule("sched_lifecycle_boom",
        "int main() { throw Exception(\"boom\"); }\n");
    VmExecutor execBoom;
    bool threw = false;
    try {
        execBoom.Execute(boom);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw, "an uncaught throw still escapes Execute");
    CHECK(execBoom.SchedulerContextCount() == 1,
          "the failed run still owns exactly main");
    CHECK(execBoom.SchedulerContext(0) != nullptr
          && execBoom.SchedulerContext(0)->state == ContextState::Failed,
          "the escaped exception marks the context Failed");
    PASS();
}

//嵌套求值窗口（运行时 toString 派发）内的回边不消耗配额：窗口属于某条
//外层指令的中途求值，那里无法安全停泊（RequestPark 会拒绝），若照常
//计数，多上下文下普通容器格式化会被配额到期错误地升级为拒绝。窗口的
//真实入口是数组→字符串强转的逐元素格式化（OP_Array_to_str →
//FormatArray → InvokeVirtualToString → CallToStringOverride）——直接的
//类→字符串强转编译为显式 toString() 调用（普通帧，非窗口），不在此列。
//控制组（普通循环）证明同一旋钮在顶层回边真实计数；实验组把全部 60 次
//回边放进 toString 体内，断言零到期。t.length()==62（60 x + 括号）同时
//证明嵌套窗口确实执行了 60 次回边（非空转）。
void test_scheduler_quota_skips_nested_windows()
{
    TEST(scheduler_quota_skips_nested_windows);
    CompiledModule plain = buildLinkedModule("sched_nest_plain",
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 60) { i = i + 1; }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor execPlain;
    execPlain.SetSchedulerTripForTests(1);
    CHECK(execPlain.Execute(plain) == 0, "control program runs");
    CHECK(execPlain.SchedulerQuotaFires() > 0,
          "control: top-level back-edges count toward the quota");
    CompiledModule nested = buildLinkedModule("sched_nest_tostring",
        "class Box {\n"
        "    public string toString() {\n"
        "        string s = \"\";\n"
        "        int i = 0;\n"
        "        while (i < 60) {\n"
        "            s = s + \"x\";\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return s;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Box[] arr = Box[1];\n"
        "    arr[0] = Box();\n"
        "    string t = arr;\n"
        "    if (t.length() != 62) return 1;\n"
        "    return 0;\n"
        "}\n");
    VmExecutor execNested;
    execNested.SetSchedulerTripForTests(1);
    CHECK(execNested.Execute(nested) == 0,
          "array coercion runs the user toString through the nested window");
    CHECK(execNested.SchedulerQuotaFires() == 0,
          "back-edges inside the toString window never consume quota");
    PASS();
}

// --- Slice 5 语言面注册（thread 命名空间 + 三内建类）---

//期望构建失败：捕获 buildLinkedModule 的诊断抛出并检查消息子串。
static bool buildFailsWith(const std::string& tag, const std::string& source,
                           const std::string& needle)
{
    try {
        buildLinkedModule(tag, source);
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

//thread.id()：main 上下文恒为 0（Slice 5 真实实现）。
void test_thread_ns_id_main()
{
    TEST(thread_ns_id_main);
    const int rc = runSource("thread_id",
        "import thread;\n"
        "int main() {\n"
        "    if (thread.id() != 0) return 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "thread.id() returns 0 on the main context");
    PASS();
}

//thread.yield()：单上下文下 park→requeue→resume 全管道往返，程序语义
//不变（每轮让与后继续，累加值精确）。
void test_thread_ns_yield_single_context()
{
    TEST(thread_ns_yield_single_context);
    const int rc = runSource("thread_yield",
        "import thread;\n"
        "int main() {\n"
        "    int s = 0;\n"
        "    int i = 1;\n"
        "    while (i <= 100) {\n"
        "        thread.yield();\n"
        "        s = s + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    if (s != 5050) return 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "100 yields round-trip through the park pipeline");
    PASS();
}

//toString 嵌套求值窗内 yield：可捕获的拒绝（不能在嵌套窗内挂起）。
//形态与 Slice 4 的嵌套窗测试相同（数组协变触发运行期元素格式化）。
void test_thread_ns_yield_in_tostring_rejected()
{
    TEST(thread_ns_yield_in_tostring_rejected);
    bool threw = false;
    std::string what;
    try {
        runSource("thread_yield_ts",
            "import thread;\n"
            "class Box {\n"
            "    public string toString() {\n"
            "        thread.yield();\n"
            "        return \"b\";\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Box[] arr = Box[1];\n"
            "    arr[0] = Box();\n"
            "    string t = arr;\n"
            "    return t.length();\n"
            "}\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "yield inside a nested evaluation window throws");
    CHECK(what.find("cannot suspend inside a nested evaluation")
              != std::string::npos,
          "rejection names the nested-evaluation rule");
    PASS();
}

//未知 thread.foo()：已导入包的未知成员走普通包诊断（与 io 缺成员同
//形——措辞不含点分前缀）。
void test_thread_ns_unknown_member()
{
    TEST(thread_ns_unknown_member);
    CHECK(buildFailsWith("thread_unknown",
        "import thread;\n"
        "int main() {\n"
        "    thread.nope();\n"
        "    return 0;\n"
        "}\n",
        "The function \"nope\" does not exist or is not accessible."),
        "unknown thread package member is a compile error naming it");
    PASS();
}

//spawn 参数形态：非函数值拒绝。
void test_thread_ns_spawn_arg_not_func()
{
    TEST(thread_ns_spawn_arg_not_func);
    CHECK(buildFailsWith("thread_sp_int",
        "import thread;\n"
        "int main() {\n"
        "    thread.spawn(42);\n"
        "    return 0;\n"
        "}\n",
        "spawn"),
        "spawn of a non-function value is a compile error");
    PASS();
}

//K3 Task 1：有返回值委托的编译面——func<T> 实参被接受（运行期行为
//属 spawn_future_* 行为测试）；带参与非函数仍拒绝（见各自测试）。
void test_thread_ns_spawn_arg_nonvoid()
{
    TEST(thread_ns_spawn_arg_nonvoid);
    bool built = true;
    std::string diag;
    try {
        buildLinkedModule("thread_sp_nv",
            "import thread;\n"
            "int f() { return 1; }\n"
            "int main() {\n"
            "    Future<int> h = thread.spawn(f);\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        built = false;
        diag = e.what();
    }
    CHECK(built, "spawn of a func<T> compiles to Future<T>: " + diag);
    PASS();
}

//spawn 参数形态：带参函数拒绝。
void test_thread_ns_spawn_arg_with_params()
{
    TEST(thread_ns_spawn_arg_with_params);
    CHECK(buildFailsWith("thread_sp_wp",
        "import thread;\n"
        "void g(int x) { }\n"
        "int main() {\n"
        "    thread.spawn(g);\n"
        "    return 0;\n"
        "}\n",
        "spawn"),
        "spawn of a parametered function is a compile error");
    PASS();
}

//spawn/join 编译面：零参 void 委托 + Thread 类型 + join 调用全部通过
//解析与发射（运行期实现属 Slice 6，此处只构建不执行）。
void test_thread_ns_spawn_compile_ok()
{
    TEST(thread_ns_spawn_compile_ok);
    bool built = true;
    std::string diag;
    try {
        buildLinkedModule("thread_sp_ok",
            "import thread;\n"
            "void worker() { }\n"
            "int main() {\n"
            "    Thread t = thread.spawn(worker);\n"
            "    thread.join(t);\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        built = false;
        diag = e.what();
    }
    CHECK(built, "spawn/join compile face resolves and emits: " + diag);
    PASS();
}

//Mutex/Channel 编译面：内建类 + 方法族解析（运行期属 Slice 8/9）。
void test_thread_mutex_channel_compile_ok()
{
    TEST(thread_mutex_channel_compile_ok);
    bool built = true;
    std::string diag;
    try {
        buildLinkedModule("thread_mc_ok",
            "int main() {\n"
            "    Mutex m = Mutex();\n"
            "    m.lock();\n"
            "    if (m.tryLock()) m.unlock();\n"
            "    Channel<int> c = {};\n"
            "    c.send(1);\n"
            "    c.close();\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        built = false;
        diag = e.what();
    }
    CHECK(built, "Mutex/Channel builtin classes resolve their methods: "
          + diag);
    PASS();
}

//包真实化：`import thread;` 后常规调用编译并运行——thread 是普通标准库
//native 包（stdlib/thread.n 承载声明，实现内建于 VM 本体）。
void test_thread_package_import()
{
    TEST(thread_package_import);
    bool ran = false;
    std::string diag;
    int rc = -1;
    try {
        rc = runSource("thread_pkg_import",
            "import thread;\n"
            "int main() {\n"
            "    if (thread.id() != 0) return 1;\n"
            "    return 0;\n"
            "}\n");
        ran = true;
    } catch (const std::exception& e) {
        diag = e.what();
    }
    CHECK(ran, "import thread resolves and thread.id() runs: " + diag);
    CHECK(rc == 0, "thread.id() returns 0 on the main context");
    PASS();
}

//包名 thread 与标准库撞名 — 通用 duplicate-package 规则接管（K1 的保留
//名门已随包真实化删除）：import 把两份同名单元拉进同一次构建，错误指
//名两条来源，与撞名 io 同形。构建在隔离子目录进行——单元文件名就是
//thread.n，落在共享 scratch 根目录会被同目录自动收编（D7）拖进后续每
//个测试的构建。
void test_thread_package_name_duplicate()
{
    TEST(thread_package_name_duplicate);
    const auto dir = scratchDir() / "dup";
    std::filesystem::create_directories(dir);
    const auto nPath = dir / "thread.n";
    {
        std::ofstream out(nPath, std::ios::binary);
        out << "import thread;\n"
               "int main() { return 0; }\n";
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = "thread";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    std::string diag;
    try {
        builder.BuildArtifacts();
    } catch (const std::exception& e) {
        diag = e.what();
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        diag += (*it)->Message() + "\n";
    std::filesystem::remove(nPath);
    std::filesystem::remove(dir / "thread.ncu");
    CHECK(diag.find("Duplicate package 'thread'") != std::string::npos,
        "a package named 'thread' collides with stdlib (both sources "
        "named): " + diag);
    PASS();
}

//包真实化准入：无 import 的 thread.id() 报包导入诊断（与 io 缺 import
//同形），不再静默走内建拦截臂。
void test_thread_requires_import()
{
    TEST(thread_requires_import);
    CHECK(buildFailsWith("thread_no_import",
        "int main() {\n"
        "    if (thread.id() != 0) return 1;\n"
        "    return 0;\n"
        "}\n",
        "Package 'thread' is not imported. Add 'import thread;'"),
        "thread.id() without import names the missing import");
    PASS();
}

//包真实化准入（typed spawn 窄臂）：无 import 的 thread.spawn(produce) 同样
//报包导入诊断——窄臂在绑定前先施与包路径相同的 import 准入判定，杜绝
//"typed spawn 静默成功而 yield 报错"的同一包两套准入行为。
void test_typed_spawn_requires_import()
{
    TEST(typed_spawn_requires_import);
    CHECK(buildFailsWith("thread_ts_no_import",
        "int produce() { return 1; }\n"
        "int main() {\n"
        "    Future<int> h = thread.spawn(produce);\n"
        "    return 0;\n"
        "}\n",
        "Package 'thread' is not imported. Add 'import thread;'"),
        "typed spawn without import names the missing import");
    PASS();
}

//m12 遮蔽优先级：用户局部名 thread 胜过内建命名空间——thread.spawn 在
//遮蔽下按普通成员链解析（对 int 报 spawn 不存在），内建面不劫持该链。
//与模块限定回退同一探针（ProbeNonFunctionField）。
void test_thread_ns_user_name_shadows()
{
    TEST(thread_ns_user_name_shadows);
    CHECK(buildFailsWith("thread_shadow",
        "int main() {\n"
        "    int thread = 5;\n"
        "    thread.spawn(1);\n"
        "    return 0;\n"
        "}\n",
        "\"spawn\" does not exist"),
        "a user local named thread shadows the namespace (m12 priority)");
    PASS();
}

//跨模块：被导入模块中的 thread.id() 调用走序列化/链接轨道。两阶段构建
//（一次构建塞两个源文件不是本 harness 的构建形态）：先单独编译 b 出
//thread_xmod_b.ncu，再编译 import 它的 a（导入自盘上 .ncu 解析），闭包
//装链接后执行——导入单元里的 OP_CallFunc 内建函数分支在链接产物上照
//常派发。
void test_thread_ns_cross_module()
{
    TEST(thread_ns_cross_module);
    const auto dir = scratchDir();
    compileUnitToDisk("thread_xmod_b",
        "import thread;\n"
        "int helper() { return thread.id(); }\n");
    const auto aNcu = compileUnitToDisk("thread_xmod_a",
        "import thread_xmod_b;\n"
        "int main() { return thread_xmod_b.helper(); }\n",
        {dir.string(), STDLIB_DIR});
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(aNcu.string(), loaderOpts);
    CompiledModule mod =
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0,
          "thread.id() inside an imported module links and runs");
    PASS();
}

// --- Slice 6 钉死测试（spawn + join + 退出策略）---

//spawn→运行→join 全管道：worker 侧唯一可观察效应经宿主 I/O 捕获。
//join 返回即证明 spawned 上下文真的执行了其根帧（Finished 只在根帧
//返回后迁移）。
void test_spawn_join_runs_worker()
{
    TEST(spawn_join_runs_worker);
    std::string out;
    const int rc = runSourceCapturing("sp_join_run",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "spawn + join program exits 0");
    CHECK(out == "w", "the spawned context ran its root frame (got: "
        + out + ")");
    PASS();
}

//轮转顺序（决策 5：FIFO + 定量配额 = 确定性调度）。配额旋钮压到 1：
//main 与 worker 的 while 各有每迭代恰一次回边，任一回边到期且另有
//Runnable 上下文即抢占。交替自 main 起（spawn 后 main 仍在载体上），
//逐字符精确钉死 "mwmwmw" —— 既证抢占真实发生，也证恢复点 pc 落在
//回边之后（重复执行或跳过迭代都会破坏串）。
void test_spawn_round_robin_under_quota()
{
    TEST(spawn_round_robin_under_quota);
    CompiledModule mod = buildLinkedModule("sp_round_robin",
        "import thread;\n"
        "import io;\n"
        "void worker() {\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        io.write(\"w\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        io.write(\"m\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetSchedulerTripForTests(1);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0, "round-robin program completes");
    CHECK(io.m_out == "mwmwmw",
          "quota 1 interleaves the two contexts exactly (got: "
          + io.m_out + ")");
    PASS();
}

//K1 终审遗留项钉板（预算残留泄漏）：自愿停泊（yield）发生在配额中途
//时，残留预算不得被下一个分派的上下文继承——CarrierLoop 每次分派重装
//满额配额，新窗口恒得完整时间片。配额 3 下主上下文烧掉 2 条回边后
//yield（残留 1）；worker 首片必须是 3 条回边（"www"）。K1 的执行器级
//单一预算（无每分派重装）下 worker 首片只有 1 条回边（"w"），输出为
//"mmwMMMwwww"；正确输出 "mmwwwMMMwww"。突变验证：注释掉 CarrierLoop
//的每分派重置行即转红。
void test_yield_park_rearms_fresh_quantum()
{
    TEST(yield_park_rearms_fresh_quantum);
    CompiledModule mod = buildLinkedModule("sp_yield_rearm",
        "import thread;\n"
        "import io;\n"
        "void worker() {\n"
        "    int i = 0;\n"
        "    while (i < 6) {\n"
        "        io.write(\"w\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    int i = 0;\n"
        "    while (i < 2) {\n"
        "        io.write(\"m\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "    thread.yield();\n"
        "    while (i < 5) {\n"
        "        io.write(\"M\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetSchedulerTripForTests(3);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0, "yield-rearm program completes");
    CHECK(io.m_out == "mmwwwMMMwww",
          "a context dispatched after a mid-quantum yield starts a FULL "
          "quota slice (got: " + io.m_out + ")");
    PASS();
}

//join 幂等：目标已 Finished 后再次 join 立即返回（不挂起、不报错）。
void test_join_idempotent_after_finish()
{
    TEST(join_idempotent_after_finish);
    std::string out;
    const int rc = runSourceCapturing("sp_join_twice",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    thread.join(t);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "double join on a finished target returns");
    CHECK(out == "w", "the worker body ran exactly once");
    PASS();
}

//join 先于目标运行：目标还在运行队列（从未执行）时 join 挂起等待，
//目标完成唤醒 joiner —— 停泊-唤醒的主通路（非抢占路径）。
void test_join_before_target_runs()
{
    TEST(join_before_target_runs);
    std::string out;
    const int rc = runSourceCapturing("sp_join_early",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "join on a never-run target completes");
    CHECK(out == "w", "the target ran to finish before join returned");
    PASS();
}

//失败目标重抛原异常对象：worker 内未捕获的异常把上下文迁移到 Failed，
//joiner 收到重抛；catch 子类形态（Boom）证明重抛的是原对象本身
//（现场新造的基类 Exception 不满足子类匹配，会作为未捕获异常逃逸）。
void test_join_failed_target_rethrows_original()
{
    TEST(join_failed_target_rethrows_original);
    std::string out;
    const int rc = runSourceCapturing("sp_join_fail",
        "import thread;\n"
        "import io;\n"
        "class Boom : Exception {\n"
        "    public Boom(string m) {\n"
        "        super(m);\n"
        "    }\n"
        "}\n"
        "void worker() { throw Boom(\"wboom\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    try {\n"
        "        thread.join(t);\n"
        "        return 4;\n"
        "    } catch (Boom e) {\n"
        "        if (e.message != \"wboom\") return 2;\n"
        "        return 0;\n"
        "    }\n"
        "}\n",
        out);
    CHECK(rc == 0,
          "join on a failed target rethrows the original exception object");
    CHECK(out.empty(), "no output side effects leak from the failure");
    PASS();
}

//绑定方法根帧：spawn 接收 func<void> 委托值，方法引用形态（接收者入
//根帧槽 0）与自由函数形态（Slice 5 既有测试）走同一 spawn 入口。
void test_spawn_bound_method_root()
{
    TEST(spawn_bound_method_root);
    std::string out;
    const int rc = runSourceCapturing("sp_bound_method",
        "import thread;\n"
        "import io;\n"
        "class Worker {\n"
        "    public int tag;\n"
        "    public void run() {\n"
        "        if (tag == 7) io.write(\"b\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Worker w = Worker();\n"
        "    w.tag = 7;\n"
        "    Thread t = thread.spawn(w.run);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "spawn of a bound method reference completes");
    CHECK(out == "b",
          "the bound root frame received its receiver (field read works)");
    PASS();
}

//委托变量形态：spawn 参数先落在局部（func<void> 局部是停泊帧的 GC
//根之一），再作为参数传入。
void test_spawn_via_delegate_variable()
{
    TEST(spawn_via_delegate_variable);
    std::string out;
    const int rc = runSourceCapturing("sp_deleg_var",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"d\"); }\n"
        "int main() {\n"
        "    func<void> f = worker;\n"
        "    Thread t = thread.spawn(f);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "spawn of a delegate-valued local completes");
    CHECK(out == "d", "the delegate's target ran");
    PASS();
}

//每上下文 id：spawned 上下文内 thread.id() ≠ 0（main 恒 0 的对侧）。
//可观察通道：id 为 0 时抛异常 → join 重抛 → main 的 catch 收到。
void test_worker_thread_id_nonzero()
{
    TEST(worker_thread_id_nonzero);
    std::string out;
    const int rc = runSourceCapturing("sp_worker_id",
        "import thread;\n"
        "import io;\n"
        "void worker() {\n"
        "    if (thread.id() == 0) throw Exception(\"main id in worker\");\n"
        "    io.write(\"k\");\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "worker observed a non-main context id");
    CHECK(out == "k", "the id check passed inside the worker");
    PASS();
}

//退出策略：main 返回即结束，未完成的 spawned 上下文静默丢弃（隐式
//join-all 会让一个睡眠者挂死所有退出——计划拍板的已知限制）。默认
//配额下 main 从不让出：worker 排队未跑，main 直接返回。
void test_main_return_drops_unfinished_worker()
{
    TEST(main_return_drops_unfinished_worker);
    std::string out;
    const int rc = runSourceCapturing("sp_drop_unfinished",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"late\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    return 7;\n"
        "}\n",
        out);
    CHECK(rc == 7, "main's return value is the run result");
    CHECK(out.empty(), "the unfinished worker is dropped silently");
    PASS();
}

//停泊上下文的 GC 根集（风险 5）：worker 循环让出，其帧（含结构体局部）
//在整个分配风暴期间停泊在调度器内；main 以压力阈值分配触发多轮回收。
//调度器根集处理缺失时结构体被清扫：槽位未复用则字段读越过 size 0 边
//界守卫当场抛错；被风暴分配复用则读到风暴值（i + 1000，刻意永不为哨兵
//42）——两条路径都让 worker 抛出、join 重抛、本测试失败。有根集处理则
//存活、"ok" 精确输出。（突变验证：注释 MarkSchedulerRoots 调用本测试
//必须转红。）
//结构三要求，缺一则空转（逐层突变验证实证）：①配额旋钮=1 强制交错——
//默认配额下 main 的 200 次回边从不抢占，worker 在 join 前从未运行；
//②worker 循环让出且圈数多于 main 迭代数——停泊窗口须横跨整个风暴，
//单次让出的 worker 在首次回收（堆超阈值 8）前就结束，窗口内零回收；
//③风暴值避开哨兵值——复用别名形只有值断言能捕获。
void test_gc_keeps_parked_context_roots()
{
    TEST(gc_keeps_parked_context_roots);
    CompiledModule mod = buildLinkedModule("sp_gc_roots",
        "import thread;\n"
        "import io;\n"
        "struct Cell {\n"
        "    public int v;\n"
        "}\n"
        "void worker() {\n"
        "    Cell c;\n"
        "    c.v = 42;\n"
        "    int spins = 0;\n"
        "    while (spins < 400) {\n"
        "        thread.yield();\n"
        "        spins = spins + 1;\n"
        "    }\n"
        "    if (c.v != 42) throw Exception(\"stale parked root\");\n"
        "    io.write(\"ok\");\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    int i = 0;\n"
        "    while (i < 200) {\n"
        "        Cell g;\n"
        "        g.v = i + 1000;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    //Quota 1 forces real interleaving: main's allocation storm runs WHILE
    //the worker sits parked (yield) with its struct live in ctx->frames —
    //the scenario this test exists for. At the default quota main's
    //200-iteration loop never preempts, the worker first runs only after
    //join, and the test would pass vacuously (mutation-verified).
    exec.SetSchedulerTripForTests(1);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "parked worker survives main's allocation storm");
    CHECK(io.m_out == "ok",
          "the parked frame's struct local stayed reachable");
    PASS();
}

//Run one test function; an escaping exception (the test's own program
//failed to build or threw at run time) counts as that test's failure,
//not a suite abort — Slice 6 tests assert on successful runs, so a
//regression must surface per-test.
//Slice 7：sleep(0) = 让出。main 在 m 与 M 之间主动让出，唯一排队的
//worker 得以在两者之间运行——纯顺序断言，不涉及时长。
void test_sleep_zero_yields()
{
    TEST(sleep_zero_yields);
    std::string out;
    const int rc = runSourceCapturing("sp_sleep_zero",
        "import thread;\n"
        "import io;\n"
        "void worker() { io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    io.write(\"m\");\n"
        "    thread.sleep(0);\n"
        "    io.write(\"M\");\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "sleep(0) program completes");
    CHECK(out == "mwM",
        "sleep(0) yields to the queued worker (got: " + out + ")");
    PASS();
}

//Slice 7：唤醒按截止时间排序而非 spawn 顺序——载体在运行队列清空后
//睡至最近到期再迁移回 Runnable。30/10ms 间隔远大于定时器分辨率；只断
//言顺序，不断言时长（计划的确定性规约）。
void test_sleep_wakes_by_deadline()
{
    TEST(sleep_wakes_by_deadline);
    std::string out;
    const int rc = runSourceCapturing("sp_sleep_order",
        "import thread;\n"
        "import io;\n"
        "void slow() { thread.sleep(30); io.write(\"a\"); }\n"
        "void fast() { thread.sleep(10); io.write(\"b\"); }\n"
        "int main() {\n"
        "    Thread t1 = thread.spawn(slow);\n"
        "    Thread t2 = thread.spawn(fast);\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "sleep program completes");
    CHECK(out == "ba",
        "earlier deadline wakes first regardless of spawn order (got: "
        + out + ")");
    PASS();
}

//Slice 7：负时长是对语言面的误用——基础异常拒绝（同 spawn 目标拒绝
//形态），消息点名约束。
void test_sleep_rejects_negative_duration()
{
    TEST(sleep_rejects_negative);
    bool threw = false;
    std::string what;
    try {
        runSource("sp_sleep_neg",
            "import thread;\n"
            "int main() {\n"
            "    thread.sleep(-1);\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "negative sleep duration must be an error");
    CHECK(what.find("non-negative") != std::string::npos,
        "message names the constraint (got: " + what + ")");
    PASS();
}

//Slice 7 审查发现（决策 2 规约 (a)）：join/sleep 在变异调度器状态后、
//RequestPark 前可能被嵌套窗口拒绝——镜像既有 yield 拒绝测试，钉死两个
//新内在函数在同一窗口的拒绝面（拒绝消息一致；规约：完成或挂起二态，
//不得半停泊）。规约本身的黑盒红灯在 Slice 10 嵌入「干净报错后执行器可
//复用」落地时才可构造。
void test_thread_ns_sleep_join_in_tostring_rejected()
{
    TEST(thread_ns_sleep_join_in_tostring_rejected);
    bool sleepThrew = false, joinThrew = false, getThrew = false;
    std::string sleepWhat, joinWhat, getWhat;
    try {
        runSource("thread_sleep_ts",
            "import thread;\n"
            "class Box {\n"
            "    public string toString() {\n"
            "        thread.sleep(1);\n"
            "        return \"b\";\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Box[] arr = Box[1];\n"
            "    arr[0] = Box();\n"
            "    string t = arr;\n"
            "    return t.length();\n"
            "}\n");
    } catch (const std::exception& e) {
        sleepThrew = true;
        sleepWhat = e.what();
    }
    try {
        runSource("thread_join_ts",
            "import thread;\n"
            "class Box {\n"
            "    public string toString() {\n"
            "        Thread u = thread.spawn(worker);\n"
            "        thread.join(u);\n"
            "        return \"b\";\n"
            "    }\n"
            "}\n"
            "void worker() { }\n"
            "int main() {\n"
            "    Box[] arr = Box[1];\n"
            "    arr[0] = Box();\n"
            "    string t = arr;\n"
            "    return t.length();\n"
            "}\n");
    } catch (const std::exception& e) {
        joinThrew = true;
        joinWhat = e.what();
    }
    try {
        runSource("future_get_ts",
            "import thread;\n"
            "class Box {\n"
            "    public string toString() {\n"
            "        Future<int> f = thread.spawn(producer);\n"
            "        f.get();\n"
            "        return \"b\";\n"
            "    }\n"
            "}\n"
            "int producer() { return 1; }\n"
            "int main() {\n"
            "    Box[] arr = Box[1];\n"
            "    arr[0] = Box();\n"
            "    string t = arr;\n"
            "    return t.length();\n"
            "}\n");
    } catch (const std::exception& e) {
        getThrew = true;
        getWhat = e.what();
    }
    CHECK(sleepThrew && joinThrew && getThrew,
        "sleep, join and future.get inside a nested evaluation window "
        "all throw");
    CHECK(sleepWhat.find("cannot suspend inside a nested evaluation")
              != std::string::npos
          && joinWhat.find("cannot suspend inside a nested evaluation")
              != std::string::npos
          && getWhat.find("cannot suspend inside a nested evaluation")
              != std::string::npos,
        "all rejections name the nested-evaluation rule (sleep: "
        + sleepWhat + "; join: " + joinWhat + "; get: " + getWhat + ")");
    PASS();
}

//Slice 8：互斥串行化（计划关键测试）。三个绑定方法 worker 在临界区内
//让出——持有期间他人的 lock 停泊、unlock 移交；无互斥时让出必然交错读
//写丢更新。断言精确终值。
void test_mutex_serializes_critical_section()
{
    TEST(mutex_serializes_critical_section);
    std::string out;
    const int rc = runSourceCapturing("mx_serial",
        "import thread;\n"
        "import io;\n"
        "class Counter {\n"
        "    public Mutex gate;\n"
        "    public int n;\n"
        "    public void bump() {\n"
        "        gate.lock();\n"
        "        n = n + 1;\n"
        "        thread.yield();\n"
        "        gate.unlock();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Counter c = Counter();\n"
        "    c.gate = Mutex();\n"
        "    Thread a = thread.spawn(c.bump);\n"
        "    Thread b = thread.spawn(c.bump);\n"
        "    Thread d = thread.spawn(c.bump);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(d);\n"
        "    io.write(c.n.toString());\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "mutex program completes");
    CHECK(out == "3", "three serialized increments (got: " + out + ")");
    PASS();
}

//Slice 8：FIFO 唤醒顺序。三个 worker 按到达顺序停泊（B 先于 D），unlock
//逐个向队首移交；临界区内各写一字母——输出恰为到达顺序。串行化测试杀
//「不停泊」突变，本测试杀「LIFO/乱序唤醒」突变（终值仍为 3、顺序错）。
void test_mutex_wakes_waiters_fifo()
{
    TEST(mutex_wakes_waiters_fifo);
    std::string out;
    const int rc = runSourceCapturing("mx_fifo",
        "import thread;\n"
        "import io;\n"
        "class Gate {\n"
        "    public Mutex m;\n"
        "    public void enterA() {\n"
        "        m.lock();\n"
        "        io.write(\"a\");\n"
        "        thread.yield();\n"
        "        m.unlock();\n"
        "    }\n"
        "    public void enterB() {\n"
        "        m.lock();\n"
        "        io.write(\"b\");\n"
        "        thread.yield();\n"
        "        m.unlock();\n"
        "    }\n"
        "    public void enterD() {\n"
        "        m.lock();\n"
        "        io.write(\"d\");\n"
        "        thread.yield();\n"
        "        m.unlock();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Gate g = Gate();\n"
        "    g.m = Mutex();\n"
        "    Thread a = thread.spawn(g.enterA);\n"
        "    Thread b = thread.spawn(g.enterB);\n"
        "    Thread d = thread.spawn(g.enterD);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(d);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "fifo program completes");
    CHECK(out == "abd",
        "waiters enter in FIFO arrival order (got: " + out + ")");
    PASS();
}

//Slice 8：tryLock 非停泊探测。worker 持锁睡 20ms；main 睡 5ms 后探测
//（持有→false），join 后再探测（已释放→true）。截止时间定序，无墙钟
//断言。
void test_mutex_trylock_reports_held_state()
{
    TEST(mutex_trylock_reports_held_state);
    std::string out;
    const int rc = runSourceCapturing("mx_trylock",
        "import thread;\n"
        "import io;\n"
        "class Holder {\n"
        "    public Mutex gate;\n"
        "    public void hold() {\n"
        "        gate.lock();\n"
        "        thread.sleep(20);\n"
        "        gate.unlock();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Holder h = Holder();\n"
        "    h.gate = Mutex();\n"
        "    Thread t = thread.spawn(h.hold);\n"
        "    thread.sleep(5);\n"
        "    if (!h.gate.tryLock()) io.write(\"1\");\n"
        "    thread.join(t);\n"
        "    if (h.gate.tryLock()) io.write(\"2\");\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "tryLock program completes");
    CHECK(out == "12",
        "held → false, released → true (got: " + out + ")");
    PASS();
}

//Slice 8：未持有 unlock 是对语言面的误用——基础异常，消息点名。
void test_mutex_unlock_without_hold_rejected()
{
    TEST(mutex_unlock_without_hold_rejected);
    bool threw = false;
    std::string what;
    try {
        runSource("mx_unlock_bare",
            "int main() {\n"
            "    Mutex m = Mutex();\n"
            "    m.unlock();\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "unlock without holding must be an error");
    CHECK(what.find("without holding") != std::string::npos,
        "message names the rule (got: " + what + ")");
    PASS();
}

//Slice 8：锁停泊帧的 GC 根集（Slice 6 形态的互斥变体）。worker 先建仅
//由自己引用的结构体局部，再对 main 持有的互斥体加锁而停泊——互斥即交
//错保证（无需配额旋钮）：停泊窗口必然横跨 main 的整个分配风暴。压力阈
//值触发多轮回收；根集处理若以停泊状态为条件跳过 ParkedMutex 帧，局部
//被清扫（越界守卫抛错或复用读到风暴值 i+1000≠42），worker 抛出、join
//重抛、本测试失败；有根集处理则 "ok" 精确输出。（突变验证：
//MarkSchedulerRoots 跳过 ParkedMutex 上下文本测试必须转红。）
void test_gc_keeps_mutex_parked_roots()
{
    TEST(gc_keeps_mutex_parked_roots);
    CompiledModule mod = buildLinkedModule("mx_gc_storm",
        "import thread;\n"
        "import io;\n"
        "struct Cell {\n"
        "    public int v;\n"
        "}\n"
        "class Gate {\n"
        "    public Mutex m;\n"
        "    public void worker() {\n"
        "        Cell mine;\n"
        "        mine.v = 42;\n"
        "        m.lock();\n"
        "        if (mine.v != 42) throw Exception(\"stale parked local\");\n"
        "        m.unlock();\n"
        "        io.write(\"ok\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Gate g = Gate();\n"
        "    g.m = Mutex();\n"
        "    g.m.lock();\n"
        "    Thread t = thread.spawn(g.worker);\n"
        "    thread.yield();\n"
        "    int i = 0;\n"
        "    while (i < 200) {\n"
        "        Cell storm;\n"
        "        storm.v = i + 1000;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    g.m.unlock();\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "parked-on-mutex worker survives main's allocation storm");
    CHECK(io.m_out == "ok",
          "the parked frame's struct local stayed reachable");
    PASS();
}

//Slice 8：不可重入——自重锁永久停泊；main join 该 worker 后运行队列与
//定时器表皆空，载体的退出守卫（ThrowIfMainUnfinished）首次可构造触发：
//「deadlock — no runnable contexts remain」。
void test_mutex_relock_deadlock_surfaces()
{
    TEST(mutex_relock_deadlock_surfaces);
    bool threw = false;
    std::string what;
    try {
        runSource("mx_relock",
            "import thread;\n"
            "class Gate {\n"
            "    public Mutex m;\n"
            "    public void relock() {\n"
            "        m.lock();\n"
            "        m.lock();\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Gate g = Gate();\n"
            "    g.m = Mutex();\n"
            "    Thread t = thread.spawn(g.relock);\n"
            "    thread.join(t);\n"
            "    return 0;\n"
            "}\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "a self-relock must surface as the carrier deadlock");
    CHECK(what.find("deadlock") != std::string::npos,
        "message names the deadlock guard (got: " + what + ")");
    PASS();
}

//Slice 9：无界缓冲 FIFO。worker 依次发送三个值；main 依序接收——缓冲区
//顺序即接收顺序。
void test_channel_fifo_buffer_order()
{
    TEST(channel_fifo_buffer_order);
    std::string out;
    const int rc = runSourceCapturing("ch_fifo",
        "import thread;\n"
        "import io;\n"
        "class Sender {\n"
        "    public Channel<string> ch;\n"
        "    public void pump() {\n"
        "        ch.send(\"a\");\n"
        "        ch.send(\"b\");\n"
        "        ch.send(\"c\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Sender s = Sender();\n"
        "    s.ch = {};\n"
        "    Thread t = thread.spawn(s.pump);\n"
        "    thread.join(t);\n"
        "    io.write(s.ch.recv());\n"
        "    io.write(s.ch.recv());\n"
        "    io.write(s.ch.recv());\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "fifo channel program completes");
    CHECK(out == "abc", "buffer drains in FIFO order (got: " + out + ")");
    PASS();
}

//Slice 9：空通道接收停泊。main 先接收（停泊），worker 的 send 追加并唤
//醒、随后写 w；main 恢复写收到的值——"wa" 顺序证明停泊先于发送。
void test_channel_recv_parks_until_send()
{
    TEST(channel_recv_parks_until_send);
    std::string out;
    const int rc = runSourceCapturing("ch_recv_park",
        "import thread;\n"
        "import io;\n"
        "class Peer {\n"
        "    public Channel<string> ch;\n"
        "    public void shout() {\n"
        "        ch.send(\"a\");\n"
        "        io.write(\"w\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Peer p = Peer();\n"
        "    p.ch = {};\n"
        "    Thread t = thread.spawn(p.shout);\n"
        "    io.write(p.ch.recv());\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "recv-park program completes");
    CHECK(out == "wa", "recv parked before send (got: " + out + ")");
    PASS();
}

//K3 Task 3：close 后缓冲先排干、随后 recv/send 均抛可捕获的
//ChannelClosedException（取代 K1 的 null/终局基础异常两套语义）。
void test_channel_closed_semantics()
{
    TEST(channel_closed_semantics);
    CompiledModule mod = buildLinkedModule("k3_chan_closed",
        "int main() {\n"
        "    Channel<int> c = {};\n"
        "    c.send(1);\n"
        "    c.close();\n"
        "    int drained = c.recv();\n"
        "    try {\n"
        "        int gone = c.recv();\n"
        "        return 1;\n"
        "    } catch (ChannelClosedException e) {\n"
        "        try {\n"
        "            c.send(2);\n"
        "            return 2;\n"
        "        } catch (ChannelClosedException e2) {\n"
        "            if (drained == 1) return 0;\n"
        "            return 3;\n"
        "        }\n"
        "    }\n"
        "}\n");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0, "closed channel: drain then throw, both sites");
    PASS();
}

//K3 Task 3：排干循环形态——缓冲 FIFO 每值恰交付一次，耗尽后以异常
//终止循环（消费者侧的惯用法）。
void test_channel_closed_drain_fifo()
{
    TEST(channel_closed_drain_fifo);
    CompiledModule mod = buildLinkedModule("k3_chan_drain",
        "int main() {\n"
        "    Channel<int> c = {};\n"
        "    int i = 0;\n"
        "    while (i < 5) { c.send(i); i = i + 1; }\n"
        "    c.close();\n"
        "    int sum = 0;\n"
        "    int got = 0;\n"
        "    bool open = true;\n"
        "    while (open) {\n"
        "        try {\n"
        "            sum = sum + c.recv();\n"
        "            got = got + 1;\n"
        "        } catch (ChannelClosedException e) {\n"
        "            open = false;\n"
        "        }\n"
        "    }\n"
        "    if (sum == 10 && got == 5) return 0;\n"
        "    return 1;\n"
        "}\n");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 0,
          "drain delivers each buffered value exactly once");
    PASS();
}

//Slice 9：close 语义。缓冲值先于 close 送入：接收先取缓冲值（close 不
//丢已缓冲数据），缓冲耗尽后接收抛 ChannelClosedException（K3 Task 3：
//取代 K1 的 null 返回——诚实的通道终点）。
void test_channel_close_drains_then_closed_throws()
{
    TEST(channel_close_drains_then_closed_throws);
    std::string out;
    const int rc = runSourceCapturing("ch_close",
        "import io;\n"
        "class Box {\n"
        "    public string tag;\n"
        "}\n"
        "int main() {\n"
        "    Channel<Box> c = {};\n"
        "    Box b = Box();\n"
        "    b.tag = \"v\";\n"
        "    c.send(b);\n"
        "    c.close();\n"
        "    io.write(c.recv().tag);\n"
        "    try {\n"
        "        c.recv();\n"
        "        return 1;\n"
        "    } catch (ChannelClosedException e) {\n"
        "        io.write(\"x\");\n"
        "        return 0;\n"
        "    }\n"
        "}\n",
        out);
    CHECK(rc == 0, "close program completes");
    CHECK(out == "vx",
        "buffered value drains, then closed recv throws (got: "
        + out + ")");
    PASS();
}

//K3 Task 3：已关闭通道再发送抛可捕获的 ChannelClosedException，消息
//点名（K1 的不可捕获基础异常升级为语言面异常——用户代码可恢复）。
void test_channel_send_after_close_catchable()
{
    TEST(channel_send_after_close_catchable);
    const int rc = runSource("ch_send_closed",
        "int main() {\n"
        "    Channel<int> c = {};\n"
        "    c.close();\n"
        "    try {\n"
        "        c.send(1);\n"
        "        return 2;\n"
        "    } catch (ChannelClosedException e) {\n"
        "        if (e.message != \"send on a closed channel\") return 3;\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "send after close throws the catchable channel exception");
    PASS();
}

//K3 Task 3：close 时仍停泊在 recv 上的等待者——缓冲交付完毕后剩余
//等待者以 ChannelClosedException 唤醒（同一异常对象；取代 K1 的静默
//null 唤醒——停泊中的消费者也必须看见通道终点）。
void test_channel_close_wakes_parked_recv_with_throw()
{
    TEST(channel_close_wakes_parked_recv_with_throw);
    std::string out;
    const int rc = runSourceCapturing("ch_close_wake",
        "import thread;\n"
        "import io;\n"
        "class Slack {\n"
        "    public Channel<int> ch;\n"
        "    public void take() {\n"
        "        try {\n"
        "            int v = ch.recv();\n"
        "            io.write(\"got\" + v);\n"
        "        } catch (ChannelClosedException e) {\n"
        "            io.write(\"closed\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Slack s = Slack();\n"
        "    s.ch = {};\n"
        "    Thread w = thread.spawn(s.take);\n"
        "    thread.yield();\n"
        "    s.ch.close();\n"
        "    thread.join(w);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "close-wake program completes");
    CHECK(out == "closed",
        "parked recv wakes with the channel-closed exception (got: "
        + out + ")");
    PASS();
}

//Slice 9：类型化往返。int 通道装箱发送/拆箱接收（接收值参与算术证拆
//箱），string 通道引用直传——两条路径同一断言输出。
void test_channel_typed_roundtrip()
{
    TEST(channel_typed_roundtrip);
    std::string out;
    const int rc = runSourceCapturing("ch_roundtrip",
        "import io;\n"
        "int main() {\n"
        "    Channel<int> ci = {};\n"
        "    ci.send(42);\n"
        "    int v = ci.recv();\n"
        "    int w = v + 1;\n"
        "    io.write(w.toString());\n"
        "    Channel<string> cs = {};\n"
        "    cs.send(\"x\");\n"
        "    io.write(cs.recv());\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "typed roundtrip program completes");
    CHECK(out == "43x", "boxed int and direct string both intact (got: "
        + out + ")");
    PASS();
}

//Slice 9：缓冲元素的 GC 存活。发送发生在已返回的 fill 帧内——装箱记录
//不残留任何活跃帧的保守带（callParam 区陈旧句柄会被过标，main 内直接发
//送则突变不可检测），缓冲是唯一可达路径，MarkChannelElements 是唯一追
//踪分支。main 的分配风暴以压力阈值触发多轮回收后逐个接收。追踪缺失则
//装箱记录被清扫：句柄悬垂，拆箱报错或读越界。（突变验证：类追踪分支跳
//过 MarkChannelElements 本测试必须转红。）
void test_channel_buffered_survives_gc_storm()
{
    TEST(channel_buffered_survives_gc_storm);
    CompiledModule mod = buildLinkedModule("ch_gc_storm",
        "import io;\n"
        "struct Cell {\n"
        "    public int v;\n"
        "}\n"
        "void fill(Channel<string> c) {\n"
        "    c.send(\"a\");\n"
        "    c.send(\"b\");\n"
        "}\n"
        "int main() {\n"
        "    Channel<string> c = {};\n"
        "    fill(c);\n"
        "    int i = 0;\n"
        "    while (i < 200) {\n"
        "        Cell storm;\n"
        "        storm.v = i + 1000;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    io.write(c.recv());\n"
        "    io.write(c.recv());\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "buffered elements survive main's allocation storm");
    CHECK(io.m_out == "ab", "both buffered strings intact (got: "
          + io.m_out + ")");
    PASS();
}

//Slice 9：裸 Channel 不再是合法类型——元素类型决定装箱与接收方拆箱，
//裸形态缓冲区会混装裸值与堆引用（必然腐化）。Channel<T> 是唯一拼写。
void test_channel_bare_name_rejected()
{
    TEST(channel_bare_name_rejected);
    CHECK(buildFailsWith("ch_bare",
        "int main() {\n"
        "    Channel c = Channel();\n"
        "    return 0;\n"
        "}\n",
        "Channel"),
        "bare Channel is rejected (generic Channel<T> is the spelling)");
    PASS();
}

//Slice 9：非空通道初始化器拒绝——值只能经 send() 进入（编译期钉死，
//消息点名规则）。
void test_channel_nonempty_init_rejected()
{
    TEST(channel_nonempty_init_rejected);
    CHECK(buildFailsWith("ch_init_nonempty",
        "int main() {\n"
        "    Channel<int> c = {1};\n"
        "    return 0;\n"
        "}\n",
        "must be empty"),
        "non-empty channel initializer is rejected with the rule pinned");
    PASS();
}

//Slice 9：嵌套求值窗口内的空通道接收——停泊门先于等待者登记（规约
//(a)：拒绝必须可捕获且不留半停泊状态），同 sleep/join/lock 形态。
void test_channel_recv_in_tostring_rejected()
{
    TEST(channel_recv_in_tostring_rejected);
    bool threw = false;
    std::string what;
    try {
        runSource("ch_recv_ts",
            "class Box {\n"
            "    public Channel<int> ch;\n"
            "    public string toString() {\n"
            "        ch.recv();\n"
            "        return \"b\";\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Box[] arr = Box[1];\n"
            "    arr[0] = Box();\n"
            "    arr[0].ch = {};\n"
            "    string t = arr;\n"
            "    return t.length();\n"
            "}\n");
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "an empty recv inside toString must be rejected");
    CHECK(what.find("cannot suspend inside a nested evaluation")
              != std::string::npos,
          "rejection names the nested-evaluation rule (got: " + what + ")");
    PASS();
}

//Slice 11：try 体内停泊、恢复后 throw 的处理器平衡（风险 2）。main 在
//try 体内 sleep 停泊（帧保留、pc 停在 try 区间内），worker 先醒写 w；
//main 恢复后写 m 并 throw——处理器搜索按 tryBlocks pc 区间，恢复点仍在
//try 体内，catch 必须接住。停泊若扰动处理器状态（合成 unwind/错位 pc），
//本测试以未捕获异常失败。
void test_park_in_try_throw_after_resume_caught()
{
    TEST(park_in_try_throw_after_resume_caught);
    std::string out;
    const int rc = runSourceCapturing("s11_try_park",
        "import thread;\n"
        "import io;\n"
        "void worker() { thread.sleep(5); io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    try {\n"
        "        thread.sleep(20);\n"
        "        io.write(\"m\");\n"
        "        throw Exception(\"boom\");\n"
        "    } catch (Exception e) {\n"
        "        io.write(\"c\");\n"
        "    }\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "park-inside-try program completes");
    CHECK(out == "wmc",
        "throw after resume lands in the enclosing catch (got: " + out
        + ")");
    PASS();
}

//Slice 11：多帧深度停泊 + 跨帧处理器搜索。worker 的内层帧在空通道
//recv 处停泊（帧栈深度 2 整体偷走），main 发送唤醒后在内层帧 throw——
//内层无 tryBlocks，弹出后须在外层帧的 try 区间命中 catch。帧栈整体保留
//与跨帧处理器走查缺一则失败。
void test_worker_recv_park_throw_cross_frame_caught()
{
    TEST(worker_recv_park_throw_cross_frame_caught);
    std::string out;
    const int rc = runSourceCapturing("s11_xframe_park",
        "import thread;\n"
        "import io;\n"
        "class Peer {\n"
        "    public Channel<int> ch;\n"
        "    public int got;\n"
        "    public void inner() {\n"
        "        got = ch.recv();\n"
        "        io.write(\"r\");\n"
        "        throw Exception(\"late\");\n"
        "    }\n"
        "    public void outer() {\n"
        "        try {\n"
        "            this.inner();\n"
        "        } catch (Exception e) {\n"
        "            io.write(\"c\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Peer p = Peer();\n"
        "    p.ch = {};\n"
        "    Thread t = thread.spawn(p.outer);\n"
        "    thread.yield();\n"
        "    p.ch.send(5);\n"
        "    thread.join(t);\n"
        "    if (p.got == 5) io.write(\"g\");\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "cross-frame park program completes");
    CHECK(out == "rcg",
        "post-resume throw crosses frames into the outer catch and the "
        "parked frame's writes survived (got: " + out + ")");
    PASS();
}

//Slice 11：finally 在停泊后的正常路径执行（计划钉死项）。main 在
//try 体内 sleep 停泊，恢复后 try 体完成——finally 入口一次、恰一次。
//顺序：worker 5ms 先醒写 w，main 20ms 醒写 m，finally 写 f。
void test_finally_runs_after_timer_park()
{
    TEST(finally_runs_after_timer_park);
    std::string out;
    const int rc = runSourceCapturing("s11_finally_park",
        "import thread;\n"
        "import io;\n"
        "void worker() { thread.sleep(5); io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(worker);\n"
        "    try {\n"
        "        thread.sleep(20);\n"
        "        io.write(\"m\");\n"
        "    } finally {\n"
        "        io.write(\"f\");\n"
        "    }\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "finally-after-park program completes");
    CHECK(out == "wmf",
        "finally runs exactly once after a parked try body (got: " + out
        + ")");
    PASS();
}

//Slice 11：worker 侧 finally——内层帧 recv 停泊（深度 2），唤醒后正常
//完成，外层 finally 照常进入。与上一测试互为镜像（停泊在 spawned 上下
//文的多帧中间）。
void test_worker_finally_after_recv_park()
{
    TEST(worker_finally_after_recv_park);
    std::string out;
    const int rc = runSourceCapturing("s11_worker_finally",
        "import thread;\n"
        "import io;\n"
        "class Peer {\n"
        "    public Channel<int> ch;\n"
        "    public void outer() {\n"
        "        try {\n"
        "            this.inner();\n"
        "        } finally {\n"
        "            io.write(\"f\");\n"
        "        }\n"
        "    }\n"
        "    public void inner() { ch.recv(); }\n"
        "}\n"
        "int main() {\n"
        "    Peer p = Peer();\n"
        "    p.ch = {};\n"
        "    Thread t = thread.spawn(p.outer);\n"
        "    thread.yield();\n"
        "    p.ch.send(1);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "worker-finally program completes");
    CHECK(out == "f",
        "finally entered once after the parked inner frame resumed and "
        "returned (got: " + out + ")");
    PASS();
}

//Slice 11：已进入的 catch 处理器体内停泊＋恢复后 rethrow。sleep 停在
//handler 体内（handlerExcStack 深度 1 随帧停泊），恢复后续跑 handler 余
//下语句，裸 throw 重抛读帧内处理器栈顶——停泊若丢失处理器状态（平衡/
//栈清空），重抛错误或落空；外层 catch 接住原异常则全链完好。
void test_park_inside_catch_body()
{
    TEST(park_inside_catch_body);
    std::string out;
    const int rc = runSourceCapturing("s11_catch_park",
        "import thread;\n"
        "import io;\n"
        "int main() {\n"
        "    try {\n"
        "        try {\n"
        "            throw Exception(\"b\");\n"
        "        } catch (Exception e) {\n"
        "            thread.sleep(5);\n"
        "            io.write(\"c\");\n"
        "            throw;\n"
        "        }\n"
        "    } catch (Exception e2) {\n"
        "        io.write(\"e\");\n"
        "    }\n"
        "    io.write(\"k\");\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "park-inside-catch program completes");
    CHECK(out == "cek",
        "the entered handler continues after its own park and its "
        "rethrow reaches the outer catch (got: " + out + ")");
    PASS();
}

//Slice 11：混合风暴（计划关键测试）。两生产者各发 25 个值（基 0 与
//100，含 0 值——装箱 0 必须与「无值」哨兵可区分），两消费者各收 25
//个，经共享互斥体折叠进同一计数对象；四个上下文循环内都分配结构体
//制造垃圾，压力阈值 8 触发多轮回收，配额 1 强制全部上下文交错停泊。
//终值与次序无关（互斥保护）：consumed=50、checksum=300+2800=3100。
//停泊根集（ParkedChanRecv/ParkedMutex/Preempted 帧）、通道缓冲元素、
//互斥等待者、装箱往返任一环缺失即失败。
void test_mixed_storm_channels_mutex_gc()
{
    TEST(mixed_storm_channels_mutex_gc);
    CompiledModule mod = buildLinkedModule("s11_storm",
        "import thread;\n"
        "import io;\n"
        "struct Cell {\n"
        "    public int v;\n"
        "}\n"
        "class Totals {\n"
        "    public int consumed;\n"
        "    public int checksum;\n"
        "}\n"
        "class Producer {\n"
        "    public Channel<int> ch;\n"
        "    public int base;\n"
        "    public void pump() {\n"
        "        int i = 0;\n"
        "        while (i < 25) {\n"
        "            Cell c;\n"
        "            c.v = base + i;\n"
        "            ch.send(base + i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "class Consumer {\n"
        "    public Channel<int> ch;\n"
        "    public Mutex gate;\n"
        "    public Totals board;\n"
        "    public void drain() {\n"
        "        int i = 0;\n"
        "        while (i < 25) {\n"
        "            int v = ch.recv();\n"
        "            Cell c;\n"
        "            c.v = v;\n"
        "            gate.lock();\n"
        "            board.consumed = board.consumed + 1;\n"
        "            board.checksum = board.checksum + v;\n"
        "            gate.unlock();\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Totals t = Totals();\n"
        "    Mutex m = Mutex();\n"
        "    Channel<int> a = {};\n"
        "    Channel<int> b = {};\n"
        "    Producer pa = Producer();\n"
        "    pa.ch = a;\n"
        "    pa.base = 0;\n"
        "    Producer pb = Producer();\n"
        "    pb.ch = b;\n"
        "    pb.base = 100;\n"
        "    Consumer ca = Consumer();\n"
        "    ca.ch = a;\n"
        "    ca.gate = m;\n"
        "    ca.board = t;\n"
        "    Consumer cb = Consumer();\n"
        "    cb.ch = b;\n"
        "    cb.gate = m;\n"
        "    cb.board = t;\n"
        "    Thread w1 = thread.spawn(pa.pump);\n"
        "    Thread w2 = thread.spawn(pb.pump);\n"
        "    Thread w3 = thread.spawn(ca.drain);\n"
        "    Thread w4 = thread.spawn(cb.drain);\n"
        "    thread.join(w1);\n"
        "    thread.join(w2);\n"
        "    thread.join(w3);\n"
        "    thread.join(w4);\n"
        "    io.write(t.consumed.toString());\n"
        "    io.write(t.checksum.toString());\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    exec.SetSchedulerTripForTests(1);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "the four-context storm completes");
    CHECK(io.m_out == "503100",
        "order-independent totals: 50 consumed, checksum 3100 (got: "
        + io.m_out + ")");
    PASS();
}

// --- K3 Task 1：Future<T> 类型面 + 有返回值 spawn + isDone ---

//spawn 依赖返回类型：func<int>/func<string> 得 Future<T>，func<void>
//仍得 Thread（K1 语义不变）；isDone 在目标完成后翻真。
void test_spawn_future_typed_and_isdone()
{
    TEST(spawn_future_typed_and_isdone);
    std::string out;
    const int rc = runSourceCapturing("k3_sp_future",
        "import thread;\n"
        "import io;\n"
        "int compute() { return 42; }\n"
        "string greet() { return \"hi\"; }\n"
        "void plain() { io.write(\"p\"); }\n"
        "int main() {\n"
        "    Future<int> fi = thread.spawn(compute);\n"
        "    Future<string> fs = thread.spawn(greet);\n"
        "    Thread t = thread.spawn(plain);\n"
        "    while (!fi.isDone()) { thread.yield(); }\n"
        "    while (!fs.isDone()) { thread.yield(); }\n"
        "    thread.join(t);\n"
        "    io.write(\"done\");\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "future program completes");
    CHECK(out == "pdone",
        "spawn dispatched both forms and isDone flipped (got: " + out + ")");
    PASS();
}

//GC 风暴下在飞 Future 的结果句柄仍被扎根（结果观察在 Task 2 的 get；
//此处先钉 isDone 轮询 + 风暴共存的编译与运行面）。
void test_spawn_future_result_survives_gc()
{
    TEST(spawn_future_result_survives_gc);
    CompiledModule mod = buildLinkedModule("k3_future_gc",
        "import thread;\n"
        "string make() {\n"
        "    string s = \"payload-\";\n"
        "    int i = 0;\n"
        "    while (i < 200) { s = s + \"x\"; i = i + 1; }\n"
        "    return s;\n"
        "}\n"
        "int main() {\n"
        "    Future<string> f = thread.spawn(make);\n"
        "    int i = 0;\n"
        "    while (i < 50) {\n"
        "        List<int> junk = {};\n"
        "        junk.add(i);\n"
        "        i = i + 1;\n"
        "    }\n"
        "    while (!f.isDone()) { thread.yield(); }\n"
        "    if (f.isDone()) return 7;\n"
        "    return 1;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(1);   //每次分配都回收——最大化风暴
    CHECK(exec.Execute(mod) == 7, "future isDone survives gc storm");
    PASS();
}

//K3 task 2：get() 的基础取值路径。main 在目标结束前调用 get——停泊在
//join 机制上，目标 OP_Return 写入 resultCell 后经唤醒投递回填取值点；
//恢复的调用不重执行（recv 同款 pc-advance 不变量）。
void test_future_get_value_and_park()
{
    TEST(future_get_value_and_park);
    std::string out;
    const int rc = runSourceCapturing("future_get_park",
        "import thread;\n"
        "import io;\n"
        "int slow() {\n"
        "    thread.yield();\n"
        "    thread.yield();\n"
        "    return 42;\n"
        "}\n"
        "int main() {\n"
        "    Future<int> f = thread.spawn(slow);\n"
        "    int v = f.get();\n"
        "    io.write(\"v=\" + v);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "get on a pending future parks and completes");
    CHECK(out == "v=42", "get returns the spawned function's result "
          "(got: " + out + ")");
    PASS();
}

//K3 task 2：引用类型 T 的取值。两重风险一起钉：①目标 Finished 后
//resultCell 独自持有字符串——spawn 时必须记录返回种类，否则 GC 根臂
//按 RTK_Int32 跳过，等待期间的回收风暴把值抽走；②二次 get 走 Finished
//快路径（同值往返，不重停泊）。spammer 在 make 完成后、main 恢复前
//持续分配——FIFO 下它先于被唤醒的 main 运行，把回收窗口拉满。
void test_future_get_string_result_survives_gc()
{
    TEST(future_get_string_result_survives_gc);
    CompiledModule mod = buildLinkedModule("future_get_gc",
        "import thread;\n"
        "string make() {\n"
        "    return \"abc\" + \"def\";\n"
        "}\n"
        "void spammer() {\n"
        "    int i = 0;\n"
        "    while (i < 100) {\n"
        "        List<int> junk = {};\n"
        "        junk.add(i);\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Future<string> f = thread.spawn(make);\n"
        "    thread.spawn(spammer);\n"
        "    string r = f.get();\n"
        "    if (r != \"abcdef\") return 1;\n"
        "    string again = f.get();\n"
        "    if (again != \"abcdef\") return 2;\n"
        "    return 7;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(1);   //每次分配都回收——最大化风暴
    CHECK(exec.Execute(mod) == 7,
          "string future result survives gc storm on both get paths");
    PASS();
}

//K3 task 2：多等待者。main 与 worker 上下文先后对同一 Future 调用
//get，都停泊在 join 机制上；目标完成时双双唤醒，各自独立取得同一值。
void test_future_get_multi_waiter()
{
    TEST(future_get_multi_waiter);
    std::string out;
    const int rc = runSourceCapturing("future_get_multi",
        "import thread;\n"
        "import io;\n"
        "int answer() {\n"
        "    thread.yield();\n"
        "    return 9;\n"
        "}\n"
        "class Holder {\n"
        "    public Future<int> f;\n"
        "    public int part;\n"
        "    public void take() {\n"
        "        part = f.get();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Holder h = Holder();\n"
        "    h.f = thread.spawn(answer);\n"
        "    Thread w = thread.spawn(h.take);\n"
        "    int mine = h.f.get();\n"
        "    thread.join(w);\n"
        "    io.write(\"v=\" + (mine + h.part));\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "two waiters on one future both complete");
    CHECK(out == "v=18", "each waiter receives the same value "
          "(got: " + out + ")");
    PASS();
}

//K3 task 2：目标失败时 get 重抛原异常对象（join 同款语义，保留消息）。
//main 先停泊、boom 后失败——覆盖唤醒侧 rethrowOnResume 路径。
void test_future_get_rethrows_target_failure()
{
    TEST(future_get_rethrows_target_failure);
    const int rc = runSource("future_get_fail",
        "import thread;\n"
        "class Boom : Exception {\n"
        "    public Boom(string m) {\n"
        "        super(m);\n"
        "    }\n"
        "}\n"
        "int boom() {\n"
        "    throw Boom(\"k3-boom\");\n"
        "}\n"
        "int main() {\n"
        "    Future<int> f = thread.spawn(boom);\n"
        "    try {\n"
        "        int v = f.get();\n"
        "        return 5;\n"
        "    } catch (Boom e) {\n"
        "        if (e.message != \"k3-boom\") return 2;\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0,
          "get on a failed future rethrows the original exception");
    PASS();
}

// --- K3 Task 4：有界通道 ---

//K3 Task 4：`Channel<int> c = Channel(2);` ——目标类型推导的裸 ctor
//invoke（计划骨架的 `new Channel<int>(2)` 无文法支撑，见台账 Ruling）。
//容量 2：两次 send 进缓冲，FIFO 取出。
void test_channel_bounded_ctor_capacity()
{
    TEST(channel_bounded_ctor_capacity);
    const int rc = runSource("chan_bounded_ctor",
        "int main() {\n"
        "    Channel<int> c = Channel(2);\n"
        "    c.send(1);\n"
        "    c.send(2);\n"
        "    int first = c.recv();\n"
        "    int second = c.recv();\n"
        "    if (first == 1 && second == 2) return 0;\n"
        "    return 1;\n"
        "}\n");
    CHECK(rc == 0, "Channel(2) constructs bounded and drains FIFO");
    PASS();
}

//K3 Task 4：容量满时 send 停泊、recv 腾位后按 FIFO 唤醒队首发送者。
//单载体确定性编排：worker 填满 cap=2 后在 send(3) 停泊（写 "s3"），
//main 每次让步后取一个值（写 "rN"），唤醒的 sender 续写 "s4"。
void test_channel_bounded_send_parks_fifo()
{
    TEST(channel_bounded_send_parks_fifo);
    std::string out;
    const int rc = runSourceCapturing("chan_bounded_fifo",
        "import thread;\n"
        "import io;\n"
        "class Pump {\n"
        "    public Channel<int> ch;\n"
        "    public void fill() {\n"
        "        ch.send(1);\n"
        "        ch.send(2);\n"
        "        io.write(\"s3\");\n"
        "        ch.send(3);\n"
        "        io.write(\"s4\");\n"
        "        ch.send(4);\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Pump p = Pump();\n"
        "    p.ch = Channel(2);\n"
        "    Thread t = thread.spawn(p.fill);\n"
        "    thread.yield();\n"
        "    io.write(\"r\" + p.ch.recv());\n"
        "    thread.yield();\n"
        "    io.write(\"r\" + p.ch.recv());\n"
        "    thread.yield();\n"
        "    io.write(\"r\" + p.ch.recv());\n"
        "    io.write(\"r\" + p.ch.recv());\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "bounded-channel program completes");
    CHECK(out == "s3r1s4r2r3r4",
          "send parks at capacity, recv wakes senders FIFO (got: "
          + out + ")");
    PASS();
}

//K3 Task 4：close 唤醒停泊中的发送者——以可捕获的
//ChannelClosedException 恢复（取代静默丢弃），未投递的待发值作废。
void test_channel_bounded_close_wakes_sender()
{
    TEST(channel_bounded_close_wakes_sender);
    std::string out;
    const int rc = runSourceCapturing("chan_bounded_close",
        "import thread;\n"
        "import io;\n"
        "class Late {\n"
        "    public Channel<int> ch;\n"
        "    public void deliver() {\n"
        "        ch.send(1);\n"
        "        try {\n"
        "            ch.send(2);\n"
        "        } catch (ChannelClosedException e) {\n"
        "            io.write(\"E\");\n"
        "            return;\n"
        "        }\n"
        "        io.write(\"x\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Late l = Late();\n"
        "    l.ch = Channel(1);\n"
        "    Thread t = thread.spawn(l.deliver);\n"
        "    thread.yield();\n"
        "    l.ch.close();\n"
        "    int drained = l.ch.recv();\n"
        "    thread.yield();\n"
        "    thread.join(t);\n"
        "    if (drained == 1) return 0;\n"
        "    return 1;\n"
        "}\n",
        out);
    CHECK(rc == 0, "close-with-parked-sender program completes");
    CHECK(out == "E", "parked sender wakes with the catchable close "
          "exception (got: " + out + ")");
    PASS();
}

//K3 Task 4：容量 0 = 无界（Python queue.Queue 惯例——与缺省同义）。
void test_channel_capacity_zero_unbounded()
{
    TEST(channel_capacity_zero_unbounded);
    const int rc = runSource("chan_cap_zero",
        "int main() {\n"
        "    Channel<string> c = Channel(0);\n"
        "    c.send(\"a\");\n"
        "    c.send(\"b\");\n"
        "    string first = c.recv();\n"
        "    string second = c.recv();\n"
        "    if (first == \"a\" && second == \"b\") return 0;\n"
        "    return 1;\n"
        "}\n");
    CHECK(rc == 0, "Channel(0) is unbounded");
    PASS();
}

//K3 Task 4：最小调用形态钉死——main 里只有 1 槽调用（close/recv 各仅
//this），`{}` 构造的容量 staging 须自成尺寸：callParam 区若只按 1 槽定
//尺寸，容量写会别名到 evalArea[0]（恰好是 init-list 句柄槽），c 变 null。
void test_channel_default_ctor_smallest_call_shape()
{
    TEST(channel_default_ctor_smallest_call_shape);
    const int rc = runSource("chan_default_minimal",
        "int main() {\n"
        "    Channel<int> c = {};\n"
        "    c.close();\n"
        "    try {\n"
        "        int v = c.recv();\n"
        "        return 1;\n"
        "    } catch (ChannelClosedException e) {\n"
        "        return 0;\n"
        "    }\n"
        "}\n");
    //已关闭空通道 recv 抛 ChannelClosedException（Task 3 收拢语义）。
    CHECK(rc == 0, "default `{}` ctor stays correct when every other "
          "call in the function takes one slot");
    PASS();
}

//K3 Task 4：负容量在构造时抛可捕获的基础 Exception（0 合法=无界，消息
//须如实区分）。
void test_channel_negative_capacity_throws()
{
    TEST(channel_negative_capacity_throws);
    const int rc = runSource("chan_cap_negative",
        "int main() {\n"
        "    try {\n"
        "        Channel<int> c = Channel(-1);\n"
        "        return 1;\n"
        "    } catch (Exception e) {\n"
        "        if (e.message == \"channel capacity must be 0 (unbounded) "
        "or at least 1\") return 0;\n"
        "        return 2;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "negative capacity raises the catchable capacity error");
    PASS();
}

//K3 Task 4：0 实参裸构造拒绝——内建 ctor invoke 是精确 arity 契约（无
//默认参数机制），漏摆容量槽会读到 evalArea 残留垃圾；无界缺省走 {}。
void test_channel_bare_ctor_no_args_rejected()
{
    TEST(channel_bare_ctor_no_args_rejected);
    CHECK(buildFailsWith("chan_no_args",
        "int main() {\n"
        "    Channel<int> c = Channel();\n"
        "    return 0;\n"
        "}\n",
        "one capacity argument"),
        "zero-arg bare channel constructor is rejected (exact staging)");
    PASS();
}

//K3 Task 4：容量实参类型门——唯一可选实参必须是 int（容量槽 4 字节
//读，其余形态编译期拒绝）。
void test_channel_capacity_non_int_rejected()
{
    TEST(channel_capacity_non_int_rejected);
    CHECK(buildFailsWith("chan_cap_type",
        "int main() {\n"
        "    Channel<int> c = Channel(\"x\");\n"
        "    return 0;\n"
        "}\n",
        "capacity must be an int"),
        "non-int capacity argument is rejected at compile time");
    PASS();
}

//K3 Task 4：待发值（pendingSends——只存在于停泊发送者的侧表队列）在
//GC 风暴下存活。cap=1：worker 填满缓冲后带 "gold!!" 停泊，main 分配
//风暴拉满回收窗口，随后两次 recv 应先取 "fill" 再取 "gold!!"。
void test_channel_bounded_pending_survives_gc()
{
    TEST(channel_bounded_pending_survives_gc);
    CompiledModule mod = buildLinkedModule("chan_bounded_gc",
        "import thread;\n"
        "import io;\n"
        "struct Cell {\n"
        "    public int v;\n"
        "}\n"
        "class Bringer {\n"
        "    public Channel<string> ch;\n"
        "    public void bring() {\n"
        "        ch.send(\"fill\");\n"
        "        ch.send(\"gold\" + \"!!\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Bringer b = Bringer();\n"
        "    b.ch = Channel(1);\n"
        "    Thread t = thread.spawn(b.bring);\n"
        "    thread.yield();\n"
        "    int i = 0;\n"
        "    while (i < 200) {\n"
        "        Cell storm;\n"
        "        storm.v = i + 1000;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    string first = b.ch.recv();\n"
        "    string gold = b.ch.recv();\n"
        "    thread.join(t);\n"
        "    if (first == \"fill\" && gold == \"gold!!\") return 0;\n"
        "    return 1;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0,
          "pending send value survives the GC storm");
    PASS();
}

void test_eval_stack_routes_to_fallback_slot()
{
    TEST(eval_stack_routes_to_fallback_slot);
    //The test thread is not a carrier: EvalStack() must route to this
    //thread's fallback slot and stay empty around a completed run.
    CompiledModule mod = buildLinkedModule("eval_slot_route",
        "int main() {\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    CHECK(exec.EvalStackProbeEmpty(), "fallback slot empty before run");
    CHECK(exec.Execute(mod) == 0, "run completes");
    CHECK(exec.EvalStackProbeEmpty(), "fallback slot empty after run");
    PASS();
}

static void runGuarded(void (*fn)())
{
    try {
        fn();
    } catch (const std::exception& e) {
        FAIL(std::string("exception: ") + e.what());
    }
}

// --- K2 Task 1 钉死测试（结果路由经调度器注册表，重构安全网）---

//同一执行器连续两次 Execute（嵌入宿主复用形态）：注册表每 run 复位、
//两次各自返回正确值。Task 1 把 main 结果路由改为经 ctx->resultCell，
//本测试钉死复用语义在重构前后不回退。
static void test_executor_reuse_two_runs()
{
    TEST(executor_reuse_two_runs);
    CompiledModule modA = buildLinkedModule("reuse_a",
        "import thread;\n"
        "int main() { thread.sleep(0); return 41; }\n");
    CompiledModule modB = buildLinkedModule("reuse_b",
        "int main() { return 1; }\n");
    VmExecutor exec;
    const int r1 = exec.Execute(modA);
    const int r2 = exec.Execute(modB);
    CHECK(r1 == 41, "first run returns 41");
    CHECK(r2 == 1, "second run returns 1");
    PASS();
}

//K2 slice 0: 全局初始化进程级线程安全——多线程并发调用 StaticInit
//（幂等）与并发 IdString 构造（命中/未命中路径交替）不得有数据竞争；
//同值字符串经两条构造路径得到的索引必须一致。
static void test_concurrent_global_init()
{
    TEST(concurrent_global_init);
    std::atomic<bool> mismatch{false};
    std::vector<std::thread> workers;
    for (int w = 0; w < 8; ++w) {
        workers.emplace_back([&, w] {
            //Concurrent re-entry of the global init: must be idempotent
            //(call_once) — the embedding host may trigger it from several
            //threads before any of them interns a string.
            Runtime::StaticInit();
            for (int k = 0; k < 200; ++k) {
                const std::string name =
                    "k2_probe_" + std::to_string((w * 31 + k) % 211);
                const IdString a(name);
                const IdString b(name.c_str());
                if (a.Index() != b.Index() || a.ToString() != name)
                    mismatch.store(true);
            }
        });
    }
    for (auto& t : workers)
        t.join();
    CHECK(!mismatch.load(), "concurrent intern mismatched indexes");
    PASS();
}

// --- K2 Task 2（辅助载体池＋多载体确定性对拍）---

//runWithCarriers：以指定载体数（主载体＋n-1 辅助线程）执行一次并捕获
//输出——多载体对拍的基础帮手。配额压到 1 强制细粒度窗口，最大化暴露
//交错差异。
static int runWithCarriers(const std::string& tag, const std::string& source,
                           uint32_t carriers, std::string& rOut)
{
    CompiledModule mod = buildLinkedModule(tag, source);
    VmExecutor exec;
    exec.SetSchedulerTripForTests(1);
    exec.SetCarrierThreadCount(carriers);
    CaptureIo io;
    exec.SetHostIo(&io);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
}

//K4 D8（原 INV-3 降级为单载体确定性档）：单载体（含库嵌入默认 1 载
//体）的窗口序确定——同一程序重复运行输出逐字节一致；多载体下调度顺
//序未指定（唤醒亲和按 lastRunner 重排），程序正确性只依赖 join/
//channel/mutex 的 happens-before。对拍：单载体重复运行逐字节一致；
//多载体重复运行须完成且输出为同一多重集（顺序无关）。
static void test_single_carrier_deterministic_multi_unspecified()
{
    TEST(single_carrier_deterministic_multi_unspecified);
    const char* src =
        "import thread;\n"
        "import io;\n"
        "void w1() {\n"
        "    int i = 0;\n"
        "    while (i < 3) { io.write(\"a\"); i = i + 1; }\n"
        "}\n"
        "void w2() {\n"
        "    int i = 0;\n"
        "    while (i < 3) { io.write(\"b\"); i = i + 1; }\n"
        "}\n"
        "int main() {\n"
        "    Thread t1 = thread.spawn(w1);\n"
        "    Thread t2 = thread.spawn(w2);\n"
        "    int i = 0;\n"
        "    while (i < 3) { io.write(\"m\"); i = i + 1; }\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    return 0;\n"
        "}\n";
    std::string out1;
    const int rc1 = runWithCarriers("carrier_parity_1", src, 1, out1);
    CHECK(rc1 == 0, "single carrier completes the program");
    size_t countM = 0, countA = 0, countB = 0;
    for (char c : out1) {
        countM += (c == 'm');
        countA += (c == 'a');
        countB += (c == 'b');
    }
    CHECK(countM == 3 && countA == 3 && countB == 3,
          "join guarantees every loop iteration ran (got: " + out1 + ")");
    CHECK(out1.find("mm") == std::string::npos
              && out1.find("aa") == std::string::npos
              && out1.find("bb") == std::string::npos,
          "quota 1 truly interleaves under a single carrier (got: "
          + out1 + ")");
    //单载体确定性档：重复运行逐字节一致。
    for (int round = 0; round < 3; ++round) {
        std::string outRepeat;
        CHECK(runWithCarriers("carrier_parity_1", src, 1, outRepeat) == 0,
              "single-carrier repeat run completes");
        CHECK(outRepeat == out1,
              "single-carrier window order is deterministic across runs "
              "(got: " + outRepeat + ", want: " + out1 + ")");
    }
    //多载体：完成＋同一多重集；顺序未指定（不再逐字节对拍）。
    std::string sorted1 = out1;
    std::sort(sorted1.begin(), sorted1.end());
    for (int round = 0; round < 5; ++round) {
        std::string out4;
        const int rc4 = runWithCarriers("carrier_parity_4", src, 4, out4);
        CHECK(rc4 == 0, "four carriers complete the program");
        std::string sorted4 = out4;
        std::sort(sorted4.begin(), sorted4.end());
        CHECK(sorted4 == sorted1,
              "multi-carrier output is a permutation of the single-"
              "carrier output — order is unspecified, the join "
              "happens-before still guarantees every iteration ran (got: "
              + out4 + ", single: " + out1 + ")");
    }
    PASS();
}

//池生命周期：同一执行器（池已创建过线程）连续两次 Execute——辅助线程
//按 run 启停、调度器状态每 run 复位，两次结果各自正确且无跨 run 残留。
static void test_carrier_pool_two_runs()
{
    TEST(carrier_pool_two_runs);
    const char* src =
        "import thread;\n"
        "import io;\n"
        "void w() { io.write(\"w\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    thread.join(t);\n"
        "    return 7;\n"
        "}\n";
    CompiledModule mod = buildLinkedModule("carrier_reuse", src);
    VmExecutor exec;
    exec.SetCarrierThreadCount(3);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 7, "first run with the pool returns 7");
    CHECK(exec.Execute(mod) == 7, "second run with the pool returns 7");
    CHECK(io.m_out == "ww", "the worker ran exactly once per run");
    PASS();
}

//多载体并非空转的白盒证明（对拍测试无法区分「4 载体真并行」与「池
//未启动的单载体退化」）：三个 worker 纯计算＋配额 1 强制细窗口，数千
//次出队让全部载体充分轮转——辅助载体分派计数必须非零；辅助载体等待
//GIL 的峰值同样必须非零。「哪个载体执行哪个窗口」在 GIL 竞争语义下
//本质非确定，本测试断言的是池的真实参与而非特定分配。
static void test_aux_carriers_actually_execute()
{
    TEST(aux_carriers_actually_execute);
    CompiledModule mod = buildLinkedModule("carrier_alive",
        "import thread;\n"
        "import io;\n"
        "void w() {\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        int j = 0;\n"
        "        while (j < 400) { j = j + 1; }\n"
        "        io.write(\"x\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread a = thread.spawn(w);\n"
        "    Thread b = thread.spawn(w);\n"
        "    Thread c = thread.spawn(w);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetSchedulerTripForTests(1);
    exec.SetCarrierThreadCount(4);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "four-carrier three-worker program completes");
    CHECK(io.m_out.size() == 9,
          "every worker iteration wrote (got: " + io.m_out + ")");
    CHECK(exec.SchedulerAuxDispatchCount() > 0,
          "contexts were dispatched on auxiliary carriers (pool is "
          "genuinely executing, not a single-carrier no-op)");
    CHECK(exec.SchedulerGilWaitersPeak() > 0,
          "auxiliary carriers contended for the GIL during the run");
    PASS();
}

// --- K5 Task 6（GIL 批 1：等待时间计数器）---

//计数器族的白盒钉板（直构调度器，镜像 test_debug_focus_exclusive_dispatch
//的形状）：竞争分类（进入等待时已有他者排队才算竞争获取）、纳秒累计、
//Reset 每 run 清零。Begin/End 只动等待者计数不动互斥本体，与 Reset 的
//自守卫获取无死锁交互；1ms sleep 保证时长断言跨时钟精度确定成立。
static void test_gil_wait_counters_whitebox()
{
    TEST(gil_wait_counters_whitebox);
    VmScheduler sched;
    const auto t1 = sched.BeginGilWait();
    CHECK(sched.GilContendedAcquires() == 0,
          "a lone waiter is not a contended acquire");
    const auto t2 = sched.BeginGilWait();
    CHECK(sched.GilContendedAcquires() == 1,
          "acquiring while another waiter is queued counts as contended");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sched.EndGilWait(t2);
    sched.EndGilWait(t1);
    CHECK(sched.GilWaitNanosTotal() > 0,
          "wait time accumulated in nanoseconds");
    sched.Reset();
    CHECK(sched.GilContendedAcquires() == 0,
          "Reset clears the contended-acquire counter");
    CHECK(sched.GilWaitNanosTotal() == 0,
          "Reset clears the wait-nanoseconds accumulator");
    PASS();
}

//端到端：多载体真实竞争下两个新计数器都非零（与既有 GilWaitersPeak>0
//同源——peak>0 即某刻 ≥2 并发等待者，后到者必计入竞争获取）。
static void test_gil_wait_time_counters()
{
    TEST(gil_wait_time_counters);
    CompiledModule mod = buildLinkedModule("gil_wait_time",
        "import thread;\n"
        "import io;\n"
        "void w() {\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        int j = 0;\n"
        "        while (j < 400) { j = j + 1; }\n"
        "        io.write(\"x\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread a = thread.spawn(w);\n"
        "    Thread b = thread.spawn(w);\n"
        "    Thread c = thread.spawn(w);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetSchedulerTripForTests(1);
    exec.SetCarrierThreadCount(4);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0,
          "four-carrier three-worker program completes");
    CHECK(exec.SchedulerGilContendedAcquires() > 0,
          "contended GIL acquisitions were counted under multi-carrier "
          "contention");
    CHECK(exec.SchedulerGilWaitNanosTotal() > 0,
          "GIL wait time accumulated under multi-carrier contention");
    PASS();
}

//单载体无竞争路径不误增：默认 1 载体、单上下文纯计算（无 spawn、无
//sleep——定时器线程懒启动不发生），主载体是唯一 GIL 获取者，竞争计数
//恒 0。不钉纳秒累计——无竞争的 lock() 也耗数十纳秒，非零不确定。
static void test_gil_wait_counters_no_contention()
{
    TEST(gil_wait_counters_no_contention);
    CompiledModule mod = buildLinkedModule("gil_wait_calm",
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 200) { i = i + 1; }\n"
        "    return 42;\n"
        "}\n");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 42,
          "single-carrier single-context program completes");
    CHECK(exec.SchedulerGilContendedAcquires() == 0,
          "no contended acquires on the single-carrier uncontended path");
    PASS();
}

// --- K4 Task 1（载体身份＋当前上下文集中登记＋调试焦点分派）---

//焦点独占分派（调度器白盒，D2）：焦点 Runnable 时它是唯一可分派上下文
//——排队中则任意出队者取得焦点（哪怕它不在队首）；焦点停泊后无状态放
//行（队首照常出队）；焦点重新入队恢复独占；焦点终结后门全开。集中登记
//表随分派/释放同步翻转。
static void test_debug_focus_exclusive_dispatch()
{
    TEST(debug_focus_exclusive_dispatch);
    VmScheduler sched;
    sched.ConfigureCarriers(1);
    sched.InstallSelfCarrier(0);
    //K7: the scheduler surface self-guards per call — drive it lock-free
    //(PopNextRunnable's cv wait needs its internal guard's own lock).
    CHECK(sched.RegisterContext() == 0, "main registers as id 0");
    CHECK(sched.RegisterContext() == 1, "first worker registers as id 1");
    CHECK(sched.RegisterContext() == 2, "second worker registers as id 2");
    CHECK(sched.PopNextRunnable() == 0,
          "no focus set: the FIFO head dispatches");
    CHECK(sched.HeldContextOfCarrier(0) == 0,
          "the held mirror records the dispatched context");
    //焦点钉在队尾：独占分派必须取焦点而非队首（纯 FIFO 会返回 1）
    sched.SetDebugFocusContext(2);
    CHECK(sched.PopNextRunnable() == 2,
          "queued focus dispatches exclusively, jumping the queue");
    CHECK(sched.HeldContextOfCarrier(0) == 2,
          "the mirror follows the focus dispatch");
    //焦点停泊（模拟步进落到空通道 recv/竞争锁）：门无状态放开
    sched.ParkForMutex(2);
    //K7：补完停泊握手——生产中停泊载体在 ParkSignal 捕获位点搬帧并
    //置 framesParked，唤醒路径只入队已完成停泊者；白盒模拟同形补一
    //步空搬，WakeForMutex 才会入队本上下文。
    sched.ParkContextFrames(2, {});
    CHECK(sched.HeldContextOfCarrier(0) == -1,
          "parking releases the held mirror slot");
    CHECK(sched.PopNextRunnable() == 1,
          "parked focus opens the gate: the head dispatches");
    sched.Requeue(1);
    sched.WakeForMutex(2);   //queue [1, 2] — focus at the tail again
    CHECK(sched.PopNextRunnable() == 2,
          "requeued focus resumes exclusivity over the head");
    //焦点终结：门开，常规分派照常（此后队列空，用关停收尾）
    sched.MarkFinished(2, 0);
    CHECK(sched.PopNextRunnable() == 1,
          "a finished focus leaves the gate open");
    //RequestStop 自守卫获取 GIL——本测试不持锁，其真实获取路径直跑。
    sched.RequestStop();
    CHECK(sched.PopNextRunnable() == -1,
          "stop ends the dequeue loop");
    PASS();
}

//焦点被持等待（held-wait，D2 的另一臂）：焦点 Runnable 且不在队列（被某
//载体的执行窗口持有）时，出队者必须等待——不得窃取队首。白盒模拟：焦点
//由本线程先行出队（生产中持有者是另一载体，规则同一条），另一线程迟滞
//RequestStop；错误实现会立即返回队首，正确实现等到关停通知返回 -1。
static void test_debug_focus_held_wait()
{
    TEST(debug_focus_held_wait);
    VmScheduler sched;
    sched.ConfigureCarriers(1);
    sched.InstallSelfCarrier(0);
    //K7: the scheduler surface self-guards per call — drive it lock-free
    //(PopNextRunnable's cv wait needs its internal guard's own lock).
    sched.RegisterContext();                //0 main
    const int focusId = sched.RegisterContext();   //1: focus-to-be
    sched.RegisterContext();                //2: head contender
    CHECK(sched.PopNextRunnable() == 0, "main dispatches first");
    CHECK(sched.PopNextRunnable() == focusId,
          "the focus context is dispatched (now held, not queued)");
    sched.SetDebugFocusContext(focusId);    //Runnable + held: gate closed
    std::thread stopper([&sched] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sched.RequestStop();
    });
    const int got = sched.PopNextRunnable();
    stopper.join();
    CHECK(got == -1,
          "focus held by a window: the dequeue waits for its release "
          "(stop ends it) instead of stealing the head");
    PASS();
}

//集中登记表（held 镜像）的运行期归属与收尾：多载体真实运行后（池已汇
//合、全部载体退出），每条载体槽必须归 -1——任何窗口结束路径（停泊/终
//结/重新入队）都释放了登记。threads 列表（Task 3）以本表为数据源。
static void test_held_context_mirror_reset_after_run()
{
    TEST(held_context_mirror_reset_after_run);
    const char* src =
        "import thread;\n"
        "import io;\n"
        "void w() {\n"
        "    int i = 0;\n"
        "    while (i < 3) {\n"
        "        int j = 0;\n"
        "        while (j < 400) { j = j + 1; }\n"
        "        io.write(\"x\");\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread a = thread.spawn(w);\n"
        "    Thread b = thread.spawn(w);\n"
        "    Thread c = thread.spawn(w);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    return 0;\n"
        "}\n";
    CompiledModule mod = buildLinkedModule("k4_mirror_reset", src);
    VmExecutor exec;
    exec.SetSchedulerTripForTests(1);
    exec.SetCarrierThreadCount(4);
    CaptureIo io;
    exec.SetHostIo(&io);
    CHECK(exec.Execute(mod) == 0, "four-carrier three-worker program ends");
    bool allReleased = true;
    for (int carrier = 0; carrier < 4; ++carrier)
        allReleased = allReleased
            && exec.SchedulerHeldContextOfCarrier(carrier) == -1;
    CHECK(allReleased,
          "after the pool joins, no carrier still holds a context");
    PASS();
}

//K2 Task 3：定时器唤醒＋死锁非误报。三个 sleeper 错峰睡眠（10/30/50
//毫秒），一个忙循环 worker 与之并发；main 在 join 处停泊——此刻全部
//载体空闲、仅有定时器在飞。断言：sleeper 依到期顺序完成（错峰保证确
//定序）、忙循环在睡眠窗口内穿插、程序正常返回。定时器唤醒缺失（sleeper
//永不清醒→join 挂起或死锁报告）或「有定时器在飞仍误报死锁」都会失败。
static void test_timer_wake_under_multi_carrier()
{
    TEST(timer_wake_under_multi_carrier);
    std::string out;
    const int rc = runWithCarriers("timer_wake_4",
        "import thread;\n"
        "import io;\n"
        "void s0() { thread.sleep(10); io.write(\"0\"); }\n"
        "void s1() { thread.sleep(30); io.write(\"1\"); }\n"
        "void s2() { thread.sleep(50); io.write(\"2\"); }\n"
        "void busy() {\n"
        "    int i = 0;\n"
        "    while (i < 20000) { i = i + 1; }\n"
        "    io.write(\"b\");\n"
        "}\n"
        "int main() {\n"
        "    Thread t0 = thread.spawn(s0);\n"
        "    Thread t1 = thread.spawn(s1);\n"
        "    Thread t2 = thread.spawn(s2);\n"
        "    Thread tb = thread.spawn(busy);\n"
        "    thread.join(tb);\n"
        "    thread.join(t0);\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    return 0;\n"
        "}\n",
        4, out);
    CHECK(rc == 0, "four-carrier program with sleepers completes");
    const size_t npos = std::string::npos;
    const size_t p0 = out.find('0'), p1 = out.find('1'),
                 p2 = out.find('2'), pb = out.find('b');
    CHECK(out.size() == 4 && p0 != npos && p1 != npos && p2 != npos
              && pb != npos,
          "exactly four writes landed (got: " + out + ")");
    CHECK(p0 < p1 && p1 < p2,
          "staggered sleepers complete in deadline order (got: " + out
          + ")");
    CHECK(pb < p2,
          "the busy worker finished inside the sleep window (got: " + out
          + ")");
    PASS();
}

// --- K4 Task 4（lastRunner 唤醒亲和，队首常数窗 D7）---

//偏好出队（调度器白盒）：队首常数窗内，载体优先取 lastCarrier==自己
//的最早项——越过非偏好队首（纯 FIFO 会返回队首）。戳经真实分派盖
//（DispatchedToCarrier 是唯一落账点），同一线程换装不同载体编号纯为
//白盒模拟「另一载体先前跑过它」。
static void test_affinity_prefers_last_runner()
{
    TEST(affinity_prefers_last_runner);
    VmScheduler sched;
    sched.ConfigureCarriers(2);
    sched.InstallSelfCarrier(0);
    //K7: the scheduler surface self-guards per call — drive it lock-free
    //(PopNextRunnable's cv wait needs its internal guard's own lock).
    CHECK(sched.RegisterContext() == 0, "main registers as id 0");
    CHECK(sched.RegisterContext() == 1, "worker 1 registers");
    CHECK(sched.RegisterContext() == 2, "worker 2 registers");
    //载体 0 连续分派全部三个上下文（队首恒为无偏好 255 → 窗内最早
    //命中即队首，纯 FIFO 不变），全部盖上载体 0 的戳。
    CHECK(sched.PopNextRunnable() == 0, "carrier 0 dispatches 0");
    CHECK(sched.PopNextRunnable() == 1, "carrier 0 dispatches 1");
    CHECK(sched.PopNextRunnable() == 2, "carrier 0 dispatches 2");
    sched.Requeue(0);
    sched.Requeue(1);   //queue [0(c0), 1(c0)]
    //载体 1 分派队首 0（窗内无自己的戳，回退队首）——0 改盖载体 1 戳。
    sched.InstallSelfCarrier(1);
    CHECK(sched.PopNextRunnable() == 0,
          "carrier 1 falls back to the head (no own stamp in window)");
    sched.Requeue(0);   //queue [1(c0), 0(c1)]
    //偏好分派：载体 1 的窗扫描命中位置 1 的 0(c1)——越过队首 1(c0)。
    //纯 FIFO（与未实现亲和的现状）返回 1；亲和实现必须返回 0。
    CHECK(sched.PopNextRunnable() == 0,
          "carrier 1 takes its own previously-run context, jumping the "
          "head stamped for another carrier");
    PASS();
}

//K 窗有界（D7）：偏好扫描只覆盖队首 AFFINITY_SCAN_WINDOW 项——自己
//的戳落在窗口之外（位置 8，前缀 8 项全是他载体戳）时必须回退队首；
//无界扫描实现会越过前缀取到它。命中计数同时钉观察面。
static void test_affinity_window_fallback()
{
    TEST(affinity_window_fallback);
    VmScheduler sched;
    sched.ConfigureCarriers(2);
    sched.InstallSelfCarrier(0);
    //K7: the scheduler surface self-guards per call — drive it lock-free
    //(PopNextRunnable's cv wait needs its internal guard's own lock).
    for (int id = 0; id < 10; ++id)
        CHECK(sched.RegisterContext() == id, "context registers in id order");
    //载体 0 逐个分派全部 10 个上下文（全为无偏好 255 → 恒取队首），
    //全部盖上载体 0 的戳。
    for (int id = 0; id < 10; ++id)
        CHECK(sched.PopNextRunnable() == id,
              "carrier 0 drains the queue in FIFO order");
    //重建队列 [1..8](c0 戳)，随后载体 1 分派队首 1（回退）——被盖 c1
    //戳的是 1。再回队 9(c0) 与 1(c1)：[2..8, 9, 1(c1)]，自己的戳落在
    //位置 8（第 9 项，窗口外）。
    for (int id = 1; id <= 8; ++id)
        sched.Requeue(id);              //queue [1..8] all c0
    sched.InstallSelfCarrier(1);
    CHECK(sched.PopNextRunnable() == 1,
          "carrier 1 falls back to the head (window holds no own stamp)");
    sched.Requeue(9);                   //queue [2..8, 9] all c0
    sched.Requeue(1);                   //queue [2..8, 9, 1(c1)]
    CHECK(sched.AffinityMisses() >= 2,
          "fallback dispatches are observable in the miss counter");
    //窗口覆盖位置 0..7（8 项）：[2..8, 9] 全是 c0 戳，1(c1) 在位置 8
    //——窗口之外，必须回退队首 2；无界扫描会越过前缀取到 1。
    CHECK(sched.PopNextRunnable() == 2,
          "an own-stamped entry beyond the 8-entry window is NOT taken; "
          "the head dispatches instead");
    CHECK(sched.AffinityHits() == 0,
          "carrier 1 never exercised affinity before the window slid");
    //窗口随消费滑动：队首 2 被取走后 1(c1) 进入窗内（位置 7），下次
    //出队必须命中越队取它——位次 ≥ 窗口的项在前项被消费后即可及。
    CHECK(sched.PopNextRunnable() == 1,
          "the own-stamped entry enters the window once the head is "
          "consumed — affinity takes it");
    CHECK(sched.AffinityHits() == 1,
          "the sliding-window hit is observable in the hit counter");
    PASS();
}

//单载体恒等纯 FIFO（D7 退化论证＋D8 确定性档）：无偏好项（255）视作
//匹配任意扫描者——单载体下一切项都命中，最早命中恒为队首。钉板：
//从未分派的上下文排在队首、已盖戳项在其后时，单载体必须取队首（仅
//精确匹配的实现会越过它取到后面的已戳项）。
static void test_affinity_single_carrier_fifo_identity()
{
    TEST(affinity_single_carrier_fifo_identity);
    VmScheduler sched;
    sched.ConfigureCarriers(1);
    sched.InstallSelfCarrier(0);
    //K7: the scheduler surface self-guards per call — drive it lock-free
    //(PopNextRunnable's cv wait needs its internal guard's own lock).
    CHECK(sched.RegisterContext() == 0, "context 0 registers");
    CHECK(sched.RegisterContext() == 1, "context 1 registers");
    CHECK(sched.PopNextRunnable() == 0, "context 0 dispatches (stamp c0)");
    CHECK(sched.PopNextRunnable() == 1, "context 1 dispatches (stamp c0)");
    CHECK(sched.RegisterContext() == 2,
          "context 2 registers after the queue drained (255, no stamp)");
    sched.Requeue(0);   //queue [2(255), 0(c0)]
    CHECK(sched.PopNextRunnable() == 2,
          "the never-dispatched head dispatches — single carrier stays "
          "pure FIFO (exact-match-only affinity would jump it to 0)");
    CHECK(sched.PopNextRunnable() == 0,
          "the stamped entry follows in FIFO order");
    PASS();
}

//辅助载体异常汇合（Review Focus #3）：死锁检测在最后一个即将空闲的
//载体上触发——可能是主载体（直接传播）也可能是辅助载体（异常进汇合
//槽，主载体在汇合点唯一重抛）。无论哪条路径，Execute 抛出的错误与消
//息必须一致；多轮重复以覆盖两条路径。
static void test_aux_carrier_error_confluence()
{
    TEST(aux_carrier_error_confluence);
    const char* src =
        "import thread;\n"
        "class Gate {\n"
        "    public Mutex m;\n"
        "    public void relock() {\n"
        "        m.lock();\n"
        "        m.lock();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Gate g = Gate();\n"
        "    g.m = Mutex();\n"
        "    Thread t = thread.spawn(g.relock);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n";
    for (int round = 0; round < 5; ++round) {
        bool threw = false;
        std::string what;
        try {
            std::string out;
            runWithCarriers("carrier_deadlock_" + std::to_string(round),
                            src, 4, out);
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
        }
        CHECK(threw,
              "the deadlock surfaces through Execute in every round");
        CHECK(what.find("deadlock") != std::string::npos,
              "message names the deadlock guard regardless of which "
              "carrier detected it (got: " + what + ")");
    }
    PASS();
}

// --- K2 Task 4（声明式阻塞原生协议，ABI v5）---

//进程内注册的慢原生（阻塞通道）：函数体真实睡眠——只有协议（暂存实参/
//结果、窃帧停泊、GIL 释放）使它们可被安全调用。nbEchoSlow 刻意先铸造
//结果再睡：句柄在暂存结果格里暴露于整个阻塞窗口，正是地址形态根必须
//存活的形状；铸造本身又经值形态根（RootMintedNativeHandle）覆盖。
static void NbWait(NativeHost*, uint8_t*, const uint8_t* args, int)
{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(native::ArgInt(args, 0)));
}

static void NbEchoSlow(NativeHost* host, uint8_t* ret,
                       const uint8_t* args, int)
{
    const std::string s = native::ArgString(host, args, 1);
    native::ReturnString(host, ret, "echo:" + s);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(native::ArgInt(args, 0)));
}

//runBlockingNative：注册两个阻塞原生后执行，捕获输出；gcStress 非零时
//压低 GC 阈值制造分配风暴。
static int runBlockingNative(const std::string& tag,
                             const std::string& source, uint32_t carriers,
                             std::string& rOut, size_t gcStress = 0)
{
    CompiledModule mod = buildLinkedModule(tag, source);
    VmExecutor exec;
    exec.SetCarrierThreadCount(carriers);
    if (gcStress)
        exec.SetGcStressThresholds(gcStress);
    CaptureIo io;
    exec.SetHostIo(&io);
    //注册键 = <TU 名>.<名>（自由函数的 VM 键带包前缀，同
    //test_native_host 的 RegisterHs 模式）。
    exec.RegisterNativeBlocking(tag + ".nbWait", &NbWait);
    exec.RegisterNativeBlocking(tag + ".nbEchoSlow", &NbEchoSlow);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
}

// --- K6 Task 4（D9：游离载体运行世代守卫）---
//run 1 的 worker 载体在 3s 阻塞原生里时 run 1 收尾：join 宽限 2s 不
//足 → 线程脱离成僵尸，其帧仍在已被 Reset 清掉的旧上下文里。run 2
//复用同一执行器（镜像 test_executor_reuse_two_runs 的复用形态），睡
//1.5s 后做分配搅动＋输出——僵尸在 t≈3s 于 run 2 中途醒来，世代比对
//不匹配 → 静默退出（不碰 run 2 的原生根、不解引用已销毁上下文）。
//守卫缺失（旧代码）：resize(watermark) 以 run-1 的陈旧水位截断 run 2
//的原生根向量 + 悬垂 ctx 解引用——ASan 下必炸，普通构建下可能静默
//腐蚀；本测试因此是防回归钉＋ASan 腿的普通构建半边（普通构建下旧
//代码也可能恰好通过——真正的红由 ASan 构建承担）。
//时序余量账：宽限 2s < 原生 3s（必脱离）；run 2 主睡 1.5s + 搅动
//≥1s ⇒ run 2 在 t≈3s 僵尸醒来时仍在跑（余量 ≥1s，见计划 Review
//Focus 4）。
static void test_zombie_carrier_generation_guard()
{
    TEST(zombie_carrier_generation_guard);
    //Run 1: worker parks in a 3s blocking native; main returns at once.
    CompiledModule modA = buildLinkedModule("zomb_guard_a",
        "import thread;\n"
        "native void nbWait(int ms);\n"
        "void w() { nbWait(3000); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    return 7;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(2);
    exec.RegisterNativeBlocking("zomb_guard_a.nbWait", &NbWait);
    CaptureIo ioA;
    exec.SetHostIo(&ioA);
    CHECK(exec.Execute(modA) == 7, "run 1 completes (worker stranded)");

    //Run 2 on the same executor: sleep past the zombie's wake, churn,
    //output, exit 9. The zombie wakes mid-run-2 (needs a GIL-free
    //moment — the sleep parks main, so the GIL is away).
    CompiledModule modB = buildLinkedModule("zomb_guard_b",
        "import thread;\n"
        "import io;\n"
        "int main() {\n"
        "    thread.sleep(1500);\n"
        "    int i = 0;\n"
        "    while (i < 30000) {\n"
        "        string garbage = \"z\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    io.write(\"run2-done\");\n"
        "    return 9;\n"
        "}\n");
    CaptureIo ioB;
    exec.SetHostIo(&ioB);
    const int rc = exec.Execute(modB);
    CHECK(rc == 9, "run 2 completes with its exit code (zombie exited "
                   "silently; got rc=" + std::to_string(rc) + ")");
    CHECK(ioB.m_out.find("run2-done") != std::string::npos,
          "run 2 output intact (got: " + ioB.m_out + ")");
    PASS();
}

// --- K7 Task 4（阻塞原生停泊时执行位翻灭）---
//停泊族两形态的会合资格语义：调度器停泊（join/sleep/recv/mutex/
//channel）经 ParkSignal 穿出——执行作用域析构翻灭执行位；**阻塞原生
//停泊**的 C++ 窗框架留在栈上（RunFrames 不退出），由停泊点的
//EndExecuting 显式离册（K7 site 5）。观察点放在原生体内：睡眠后采样
//（此时 main 已停泊、worker 早已停泊于定时器睡眠），采样 0 ⇔ 本线程
//执行位已翻灭；缺 EndExecuting 则位仍立 → 采样恒 1。确定性判别，不
//用宿主线程轮询：main 进原生前的执行位仅微秒级立着，轮询命中翻转序
//列的概率≈0（实测已证伪轮询方案）。
static VmExecutor* s_winParkExec = nullptr;
static std::atomic<int> s_windowDuringNative{-1};
static void NbSampleWindow(NativeHost*, uint8_t*, const uint8_t* args, int)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(
        native::ArgInt(args, 0)));
    s_windowDuringNative.store(s_winParkExec->GcExecutingSelf() ? 1 : 0);
}
static void test_executing_bit_off_while_native_blocked()
{
    TEST(executing_bit_off_while_native_blocked);
    CompiledModule mod = buildLinkedModule("win_parked",
        "import thread;\n"
        "native void nbSample(int ms);\n"
        "void w() { thread.sleep(100000); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    nbSample(300);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(2);
    s_winParkExec = &exec;
    s_windowDuringNative.store(-1);
    exec.RegisterNativeBlocking("win_parked.nbSample", &NbSampleWindow);
    CHECK(exec.Execute(mod) == 0, "blocking-native program completes");
    CHECK(s_windowDuringNative.load() == 0,
        "the blocking-native park flips the executing bit off (got "
        + std::to_string(s_windowDuringNative.load()) + ")");
    s_winParkExec = nullptr;
    PASS();
}

// --- K6 Task 5（D7：收集计数器——压力下收集确实发生）---
static void test_gc_collection_count_under_stress()
{
    TEST(gc_collection_count_under_stress);
    CompiledModule mod = buildLinkedModule("gc_count",
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 20000) {\n"
        "        string garbage = \"g\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0, "stress program completes");
    CHECK(exec.GcCollectionCount() > 100,
        "collections actually ran under stress (got "
        + std::to_string(exec.GcCollectionCount()) + ")");
    PASS();
}

// --- K7 Task 4（stop 路径后世代容忍完好）---
//run 1 的 worker 在 3s 阻塞原生里时 run 1 收尾（宽限 2s 不够 → 脱离
//成僵尸）；run 2 复用同一执行器：main 睡 1.5s 后进 400ms 采样原生。
//僵尸在 t≈3s 于 run 2 中途醒来：FinishBlockingNative 的 BeginExecuting
//命中的是已被 Reset 退役的登记条目 → 僵尸容忍返回 false（不等待、不
//复活）；世代守卫抛 ShutdownSignal 静默退出——全程不触碰 run 2 的登
//记表。run 2 的采样点（t≈4s，僵尸已退出）读 0 ⇔ 无幽灵残留；若退役
//不彻底或令牌复用错位，僵尸的苏醒会触发绊线终止或污染 run 2 的会合
//分母——两侧失衡都被「run 2 完整跑完＋采样 0」钉住。
static void test_executing_bit_intact_after_stop_path()
{
    TEST(executing_bit_intact_after_stop_path);
    CompiledModule modA = buildLinkedModule("win_stop_a",
        "import thread;\n"
        "native void nbWait(int ms);\n"
        "void w() { nbWait(3000); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(2);
    exec.RegisterNativeBlocking("win_stop_a.nbWait", &NbWait);
    CHECK(exec.Execute(modA) == 0,
        "run 1 completes (worker stranded mid-native)");
    CompiledModule modB = buildLinkedModule("win_stop_b",
        "import thread;\n"
        "native void nbSample(int ms);\n"
        "void w() { thread.sleep(100000); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    thread.sleep(1500);\n"
        "    nbSample(400);\n"
        "    return 0;\n"
        "}\n");
    s_winParkExec = &exec;
    s_windowDuringNative.store(-1);
    exec.RegisterNativeBlocking("win_stop_b.nbSample", &NbSampleWindow);
    CHECK(exec.Execute(modB) == 0, "run 2 completes");
    CHECK(s_windowDuringNative.load() == 0,
        "run 2's rendezvous state is clean after run 1's zombie exited "
        "via the stop path (got "
        + std::to_string(s_windowDuringNative.load()) + ")");
    s_winParkExec = nullptr;
    PASS();
}

// --- K7 Task 4（嵌套求值窗不另立会合资格）---
//数组→字符串强转经 FormatArray→toString 覆写开**嵌套 RunFrames**（同
//线程深度 2）——嵌套窗体内非阻塞原生直读本线程执行位必须仍为 1（分
//派执行作用域早已立位，嵌套窗不新立资格；K7 删除了 K6 的窗记账——
//位是每线程单份），run 结束后归 0。嵌套窗形态仿 test_scheduler_quota_
//skips_nested_windows 的强转嵌套窗；探针走 RegisterNative 非阻塞路径
//（CallNative 原地直呼，无窗口转换）。NativeFn 签名与返回值写法以
//include/nlang/vm/NativeHost.h＋test_native_host.cpp 的 HsMint 形态为
//准（返回 void，经 native::ReturnInt 写结果格）。
static VmExecutor* s_windowProbeExec = nullptr;
static void WindowProbe(NativeHost*, uint8_t* ret,
                        const uint8_t*, int)
{
    native::ReturnInt(ret, s_windowProbeExec->GcExecutingSelf() ? 1 : 0);
}
static void test_nested_window_single_membership()
{
    TEST(nested_window_single_membership);
    CompiledModule mod = buildLinkedModule("win_nested",
        "class Box {\n"
        "    public string toString() {\n"
        "        int n = winprobe();\n"
        "        if (n != 1) return \"bad\";\n"
        "        return \"ok\";\n"
        "    }\n"
        "}\n"
        "native int winprobe();\n"
        "int main() {\n"
        "    Box[] arr = Box[1];\n"
        "    arr[0] = Box();\n"
        "    string t = arr;\n"
        "    if (t != \"[ok]\") return 1;\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    s_windowProbeExec = &exec;
    exec.RegisterNative("win_nested.winprobe", &WindowProbe);
    CHECK(!exec.GcExecutingSelf(), "not executing before the run");
    CHECK(exec.Execute(mod) == 0,
        "nested toString window keeps the single executing bit (depth 2, "
        "one membership)");
    s_windowProbeExec = nullptr;
    CHECK(!exec.GcExecutingSelf(),
        "executing bit returns to off after the run");
    PASS();
}

// --- K6 Task 6（D3：堆侧阈值回退）---
//高搅动小活集：越过 1024 首收集后自由表覆盖分配，size 停在阈值+1，
//回退 max(threshold, size) 令触发谓词翻回假——收集节奏＝自由表耗尽
//（≈每 ~1000 次分配一次）。旧行为（无回退）：size 恒 > 阈值 ⇒ 每个
//安全点都收集（数千次）。
static void test_backoff_heap_after_cliff()
{
    TEST(backoff_heap_after_cliff);
    CompiledModule mod = buildLinkedModule("bo_heap",
        "class P { public int v; }\n"
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 5000) {\n"
        "        P p = P();\n"
        "        p.v = i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.ResetGcThresholds();
    CHECK(exec.Execute(mod) == 0, "churn program completes");
    const size_t collections = exec.GcCollectionCount();
    CHECK(collections <= 20,
        "post-cliff collection cadence is free-list exhaustion, not "
        "every-safepoint (got " + std::to_string(collections) + ")");
    CHECK(collections >= 1, "the cliff itself did collect at least once");
    //D7 consumer: bounded liveness — the store recycles slots in place
    //(size never shrinks), so the kind!=free population is bounded by
    //the backed-off threshold (~1025) plus the unswept inter-collection
    //tail; 5000 dead P objects never mean 5000 occupied slots (the
    //no-GC shape). NOT <100: the tail since the last back-edge sweep
    //is itself dead-but-unswept (~900 slots here) and counts nonzero.
    const size_t liveSlots = exec.LiveHeapSlotCount();
    CHECK(liveSlots < 1200,
        "heap slot population stays bounded under churn (got "
        + std::to_string(liveSlots) + ")");
    PASS();
}

// --- K6 Task 6（D3：字符串侧对称回退）---
//旧回退 max(threshold, 2*live) 在小活集搅动下同样每安全点收集
//（live≈2 ⇒ 阈值永不抬升、size 恒越界）。max(threshold, size) 同治。
static void test_backoff_string_symmetric()
{
    TEST(backoff_string_symmetric);
    CompiledModule mod = buildLinkedModule("bo_str",
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 5000) {\n"
        "        string garbage = \"g\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.ResetGcThresholds();
    CHECK(exec.Execute(mod) == 0, "string churn program completes");
    const size_t collections = exec.GcCollectionCount();
    CHECK(collections <= 20,
        "string store backs off symmetrically (got "
        + std::to_string(collections) + ")");
    CHECK(collections >= 1, "crossed the string threshold at least once");
    PASS();
}

// --- K6 Task 6（D3：压力模式高节奏）---
//压力＝钉小初始阈值；回退照常运行（pre-K6 语义，deep_chain e2e 钉住
//计时）。size 臂主导下每收集周期 size 仅 +1、节奏＝自由表耗尽＝size，
//算术步进 ⇒ 5000 次分配约 √(2N)≈100 次收集——远高于常规模式同型程序
//（≤20 次），又非每安全点强制。
static void test_stress_keeps_high_cadence()
{
    TEST(stress_keeps_high_cadence);
    CompiledModule mod = buildLinkedModule("bo_stress",
        "class P { public int v; }\n"
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 5000) {\n"
        "        P p = P();\n"
        "        p.v = i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.ResetGcThresholds();
    exec.SetGcStressThresholds(8);
    CHECK(exec.Execute(mod) == 0, "stress churn program completes");
    CHECK(exec.GcCollectionCount() > 50,
        "stress keeps a high collection cadence (got "
        + std::to_string(exec.GcCollectionCount()) + ")");
    PASS();
}

// --- K6 Task 6（D3：阈值跨 run 持久）---
//执行器复用（嵌入宿主形态）：run 1 搋动后阈值已抬到高水位；run 2
//微量分配不得再收集。m_gcCollections 每 run 清零（Task 5），断言的是
//run 2 自己的计数。
static void test_thresholds_persist_across_runs()
{
    TEST(thresholds_persist_across_runs);
    CompiledModule churn = buildLinkedModule("bo_persist1",
        "class P { public int v; }\n"
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 5000) {\n"
        "        P p = P();\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.ResetGcThresholds();
    CHECK(exec.Execute(churn) == 0, "run 1 churn completes");
    CompiledModule tiny = buildLinkedModule("bo_persist2",
        "class P { public int v; }\n"
        "int main() {\n"
        "    P a = P();\n"
        "    P b = P();\n"
        "    int i = 0;\n"
        "    while (i < 100) { i = i + 1; }\n"
        "    return a.v + b.v;\n"
        "}\n");
    CHECK(exec.Execute(tiny) == 0, "run 2 tiny completes");
    //The loop's back-edges are safepoints that would collect at every
    //iteration if the threshold had reset to the default (size stays at
    //run 1's high-water mark, above 1024); persistence keeps the
    //predicate false — zero collections. The two fresh allocations
    //reuse free-list slots, so size never crosses the backed-off mark.
    CHECK(exec.GcCollectionCount() == 0,
        "backed-off threshold survives into the reused run (got "
        + std::to_string(exec.GcCollectionCount()) + ")");
    PASS();
}

// --- K6 Task 6（D3：复位旋钮恢复默认行为）---
static void test_reset_gc_thresholds_restores_defaults()
{
    TEST(reset_gc_thresholds_restores_defaults);
    VmExecutor exec;
    exec.SetGcStressThresholds(4);
    exec.ResetGcThresholds();
    //A default-threshold tiny program must not collect: proves the
    //stress clamp is gone (a stale clamp would collect on the second
    //allocation).
    CompiledModule mod = buildLinkedModule("bo_reset",
        "class P { public int v; }\n"
        "int main() {\n"
        "    P a = P();\n"
        "    return a.v;\n"
        "}\n");
    CHECK(exec.Execute(mod) == 0, "tiny program completes");
    CHECK(exec.GcCollectionCount() == 0,
        "defaults restored — no collection, no stress residue");
    PASS();
}

//协议判别器 1（GIL 释放）：main 阻塞 150ms，worker 在窗口内抢到 GIL 并
//输出。main 从 spawn 到阻塞调用之间无回边（不会被配额抢占），故 worker
//的输出先于 main-after 只能来自「阻塞期间 GIL 离手」；不释放则顺序反转
//（worker 只能在 main 返回后运行）——测试变红。
static void test_blocking_native_releases_gil()
{
    TEST(blocking_native_releases_gil);
    std::string out;
    const int rc = runBlockingNative("bn_gil",
        "import thread;\n"
        "import io;\n"
        "native void nbWait(int ms);\n"
        "void w() { io.write(\"worker|\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    nbWait(150);\n"
        "    io.write(\"main-after|\");\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        2, out);
    CHECK(rc == 0, "blocking-native program completes");
    const size_t pw = out.find("worker|");
    const size_t pm = out.find("main-after|");
    CHECK(pw != std::string::npos && pm != std::string::npos,
          "both outputs landed (got: " + out + ")");
    CHECK(pw < pm,
          "the worker ran inside the blocking window — the GIL was "
          "released (got: " + out + ")");
    PASS();
}

//协议判别器 2（暂存根存活 GC 风暴，回归针）：churner 在阻塞窗口内以压
//低阈值反复触发回收；nbEchoSlow 先把铸造好的句柄写进暂存结果格再睡，
//整个风暴期间该句柄只有原生调用根保护。根缺失 ⇒ 句柄被清扫 ⇒ 输出非
//echo:payload（或崩溃）——测试变红。
static void test_blocking_native_staged_roots_survive_gc_storm()
{
    TEST(blocking_native_staged_roots_survive_gc_storm);
    std::string out;
    const int rc = runBlockingNative("bn_gcstorm",
        "import thread;\n"
        "import io;\n"
        "native string nbEchoSlow(int ms, string s);\n"
        "void churner() {\n"
        "    int i = 0;\n"
        "    while (i < 50000) {\n"
        "        string garbage = \"g\" + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(churner);\n"
        "    string s = nbEchoSlow(150, \"payload\");\n"
        "    io.write(s);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        2, out, /*gcStress=*/64);
    CHECK(rc == 0, "GC-storm blocking-native program completes");
    CHECK(out.find("echo:payload") != std::string::npos,
          "the staged result handle survived every collection in the "
          "window (got: " + out + ")");
    PASS();
}

// --- K6 Task 3（D11：原生根按属主移除）---
//并发阻塞臂交错时，位置截断（旧 resize-to-watermark）会误杀别人的在
//飞根：A 于水位 5 压入两项、B 于水位 7 压入两项、A 先退出——旧代码
//resize(5) 杀掉 B 的两项。属主化后 A 只删自己的。本测试白盒驱动
//m_nativeCallRoots 的交错形状（产品级交错由探针 2 的 ASan 腿覆盖）。
//RemoveNativeRoots 是私有成员：经 VmExecutor.h 的 K6 测试旋钮
//（TestPushNativeRoot 族）压入/移除/断言。
static void test_native_root_owner_removal()
{
    TEST(native_root_owner_removal);
    VmExecutor exec;
    uint8_t a1[8] = {}, a2[8] = {}, b1[8] = {}, b2[8] = {};
    //Interleaved pushes: A, A, B, B — a positional watermark for A's
    //exit sits between B's entries (the shape that kills them).
    exec.TestPushNativeRoot(&a1[0], 1, 0, 101);
    exec.TestPushNativeRoot(&a2[0], 1, 0, 101);
    exec.TestPushNativeRoot(&b1[0], 1, 0, 202);
    exec.TestPushNativeRoot(&b2[0], 1, 0, 202);
    exec.TestRemoveNativeRoots(101);   // A exits first
    CHECK(exec.TestNativeRootCount() == 2,
          "only A's entries removed; B's survive");
    exec.TestRemoveNativeRoots(202);
    CHECK(exec.TestNativeRootCount() == 0, "B's removal empties the set");
    PASS();
}

//协议判别器 3（停机穿透）：worker 在阻塞原生内时 main 返回（停机请求）
//——原生返回后必须经 ShutdownSignal 丢弃该上下文：无输出、无结果消费、
//帧留 ctx、INV-2 成立。缺停机检查 ⇒ worker 复活并输出 after-native——
//测试变红。main 先阻塞 10ms 保证 worker 确实进入原生（判别前提成立）。
static void test_shutdown_signal_passthrough()
{
    TEST(shutdown_signal_passthrough);
    std::string out;
    const int rc = runBlockingNative("bn_stop",
        "import thread;\n"
        "import io;\n"
        "native void nbWait(int ms);\n"
        "void w() { nbWait(60); io.write(\"after-native|\"); }\n"
        "int main() {\n"
        "    Thread t = thread.spawn(w);\n"
        "    nbWait(10);\n"
        "    return 0;\n"
        "}\n",
        2, out);
    CHECK(rc == 0, "main returns cleanly while a worker sits blocked");
    CHECK(out.find("after-native") == std::string::npos,
          "the blocked context was abandoned at stop, before its next "
          "output (got: " + out + ")");
    PASS();
}

// --- K2 Task 5（精确根集位图，NCU minor 4）---

//同一程序在指定位带模式下执行：压低 GC 阈值制造分配风暴，返回退出码
//并带回输出。conservative=true 强制走 K1 保守双域带扫描（对拍基准）。
static bool runGcBandMode(const std::string& tag, const std::string& source,
                          size_t gcStress, bool conservative,
                          std::string& rOut)
{
    CompiledModule mod = buildLinkedModule(tag, source);
    VmExecutor exec;
    exec.SetGcStressThresholds(gcStress);
    exec.SetConservativeBandForTests(conservative);
    CaptureIo io;
    exec.SetHostIo(&io);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc == 0;
}

//位图判别源：混合三类求值带占用——嵌套调用实参（对象跨安全点存活于
//求值带，F2 原型形态）、字符串拼接、数值二元运算；循环内持续分配制
//造风暴。tag 刻意返回超过短串驻留上限（40 字节）的长串，保证停泊值
//可清扫——短串不朽会让错标不可见。四条活根各跨一次安全点：acc（命名
//StrHandle 槽）全程存活；tag("x") 的长串结果停泊在外层二元 claim 槽
//（默认 Dual 类——嵌套发射的中间值也会路过该槽，故二元位点不能按外
//层操作数类型精化）、跨 tag("y") 调用入口安全点存活；churn2 的
//Box(i+1) 实参停泊在调用实参 claim 槽（默认 Dual 类），跨 drain 调用
//入口安全点存活，且 drain 内部再分配一个 Box 并从自由表复用槽位——
//若停泊实参被错标清扫，复用会改写其载荷、c.get() 读到 i+7 而非 i+1，
//让欠标经由槽位复用变得可观测（单纯清扫后陈旧读仍返回原值）；b 在
//字符串分配之后仍被读取（命名 HeapIndex 槽）。
static const char* kGcBandSource =
    "import io;\n"
    "class Box {\n"
    "    int v;\n"
    "    public Box(int x) { v = x; }\n"
    "    public int get() { return v; }\n"
    "}\n"
    "int churn2(Box c, int k) { return c.get() * 2 + k; }\n"
    "string tag(string s) {\n"
    "    return \"0123456789012345678901234567890123456789\" + s;\n"
    "}\n"
    "int drain(Box p) {\n"
    "    Box w = Box(p.get() + 7);\n"
    "    return w.get();\n"
    "}\n"
    "int main() {\n"
    "    string acc = \"\";\n"
    "    int sum = 0;\n"
    "    int i = 0;\n"
    "    while (i < 400) {\n"
    "        Box b = Box(i);\n"
    "        acc = acc + (tag(\"x\") + tag(\"y\"));\n"
    "        sum = sum + churn2(Box(i + 1), drain(b)) + b.get();\n"
    "        i = i + 1;\n"
    "    }\n"
    "    io.print(sum);\n"
    "    io.print(acc);\n"
    "    return 0;\n"
    "}\n";

//判别器 1（对拍）：同一程序、同一风暴阈值，位图模式与保守带模式的输出
//逐字节一致、退出码一致。两类标记对同一对象图必须得出同一存活集——
//位图错标（把句柄槽标 None/单域错配） ⇒ 存活集缩小 ⇒ 悬垂句柄 ⇒ 输出
//分叉或崩溃——测试变红。
static void test_gc_bitmap_matches_conservative_band()
{
    TEST(gc_bitmap_matches_conservative_band);
    std::string outBitmap, outConservative;
    const bool okBm = runGcBandMode("gc_band_bm", kGcBandSource,
                                    /*gcStress=*/4, false, outBitmap);
    const bool okCs = runGcBandMode("gc_band_cs", kGcBandSource,
                                    /*gcStress=*/4, true, outConservative);
    CHECK(okBm && okCs, "both band modes complete the storm program");
    CHECK(!outBitmap.empty(), "the program produced output");
    CHECK(outBitmap == outConservative,
          "bitmap and conservative band scans keep the same object "
          "graph alive (bitmap: " + outBitmap + ", conservative: "
          + outConservative + ")");
    PASS();
}

//判别器 2（NCU 往返）：加载后的模块里每个函数的位图长度与其帧布局吻合
//（〔localsSize/8 − paramCount〕槽、每槽 2 位——saver 写、loader 读两侧
//对齐）；且至少存在一个非 Dual 槽——全 Dual 的位图证明不了类别分派真
//被走过（等价于保守带的恒等替换）。
static void test_gc_bitmap_survives_ncu_roundtrip()
{
    TEST(gc_bitmap_survives_ncu_roundtrip);
    CompiledModule mod = buildLinkedModule("gc_band_rt", kGcBandSource);
    bool sawPreciseSlot = false;
    for (const auto& fn : mod.functions) {
        const size_t bandSlots =
            static_cast<size_t>(fn.localsSize) / kFrameSlotBytes
            - fn.paramCount;
        const size_t expectedBytes = (bandSlots + 3) / 4;
        CHECK(fn.evalAreaHandleMask.size() == expectedBytes,
              "mask length matches the frame layout for " + fn.name);
        for (size_t i = 0; i < bandSlots; ++i) {
            const unsigned cls =
                (fn.evalAreaHandleMask[i >> 2] >> ((i & 3) * 2)) & 3u;
            if (cls != 3u)
                sawPreciseSlot = true;
        }
    }
    CHECK(sawPreciseSlot,
          "at least one band slot carries a precise class (None/"
          "StrHandle/HeapIndex), not the Dual default");
    PASS();
}

// --- K7 Task 4（真实多方会合：注册表分母＋跨载体存活＋单载体退化）---

//4 载体并发跑分配密集程序（压力阈值 4）：会合分母真实为多——某载体
//发起停止时其余载体执行中，末到者内联收集、先到者停泊等待（非退化
//形态）。断言：运行完成、终值正确、停泊计数 > 0（真实停泊路径确实
//被走到）；各 worker 的 keep 对象仅存活于「另一载体求值栈」——收集
//者的注册表扫描臂缺失时该对象被清扫，终值错误变红。
static void test_real_multiparty_rendezvous()
{
    TEST(real_multiparty_rendezvous);
    CompiledModule mod = buildLinkedModule("k7_multi_rendezvous",
        "import thread;\n"
        "class Node { public int v; }\n"
        "int wA() {\n"
        "    Node keep = Node();\n"
        "    keep.v = 77;\n"
        "    int i = 0;\n"
        "    while (i < 20000) { Node g = Node(); g.v = i; i = i + 1; }\n"
        "    return keep.v;\n"
        "}\n"
        "int wB() {\n"
        "    Node keep = Node();\n"
        "    keep.v = 88;\n"
        "    int i = 0;\n"
        "    while (i < 20000) { Node g = Node(); g.v = i; i = i + 1; }\n"
        "    return keep.v;\n"
        "}\n"
        "int wC() {\n"
        "    Node keep = Node();\n"
        "    keep.v = 99;\n"
        "    int i = 0;\n"
        "    while (i < 20000) { Node g = Node(); g.v = i; i = i + 1; }\n"
        "    return keep.v;\n"
        "}\n"
        "int main() {\n"
        "    Future<int> a = thread.spawn(wA);\n"
        "    Future<int> b = thread.spawn(wB);\n"
        "    Future<int> c = thread.spawn(wC);\n"
        "    int i = 0;\n"
        "    while (i < 2000) { Node g = Node(); g.v = i; i = i + 1; }\n"
        "    if (a.get() != 77) return 1;\n"
        "    if (b.get() != 88) return 2;\n"
        "    if (c.get() != 99) return 3;\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(4);
    exec.SetGcStressThresholds(4);
    CHECK(exec.Execute(mod) == 0,
          "multiparty rendezvous run completes with every held value "
          "intact");
    CHECK(exec.GcCollectionCount() > 0, "collections actually ran");
    CHECK(exec.GcRendezvousParks() > 0,
          "some rendezvous parked a non-last arriver (real multiparty "
          "path, got "
          + std::to_string(exec.GcRendezvousParks()) + ")");
    PASS();
}

//单载体退化形态：唯一在执行线程发起即末到（期望数=1），一切收集内
//联完成——停泊计数恒 0。若单载体路径误入停泊等待（分母多算），轻则
//计数变红，重则挂死由超时暴露。
static void test_single_carrier_inline_shape()
{
    TEST(single_carrier_inline_shape);
    CompiledModule mod = buildLinkedModule("k7_single_inline",
        "class P { public int v; }\n"
        "int main() {\n"
        "    int i = 0;\n"
        "    while (i < 20000) { P p = P(); p.v = i; i = i + 1; }\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(1);
    exec.SetGcStressThresholds(4);
    CHECK(exec.Execute(mod) == 0, "single-carrier stress run completes");
    CHECK(exec.GcCollectionCount() > 10, "collections actually ran (got "
          + std::to_string(exec.GcCollectionCount()) + ")");
    CHECK(exec.GcRendezvousParks() == 0,
          "the sole executer collects inline every time (got "
          + std::to_string(exec.GcRendezvousParks()) + ")");
    PASS();
}

//阻塞原生回调重入（已注册且未执行分支）：载体 A 的阻塞原生在睡眠后
//经 CallByName 重入字节码——该线程此刻已被 EndExecuting、帧已偷取进
//上下文；载体 B 同时搅动触发收集风暴。判别键必须是 ExecutingSelf：
//重入须重新 BeginExecuting 成为完整参与方（其增长中的帧经自身注册表
//条目被扫描、自身安全点到达），否则收集者扫描臂与回调栈并发。断
//言：不挂死、回调结果正确。
static VmExecutor* s_rendezvousCbExec = nullptr;
static std::atomic<int> s_rendezvousCbResult{-1};
static void NbReenterChurn(NativeHost*, uint8_t*,
                           const uint8_t* args, int)
{
    const int ms = native::ArgInt(args, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    uint8_t cell[8] = {};
    s_rendezvousCbExec->CallByName("k7_cb_gc.cb_body", nullptr, 0, cell);
    int32_t v = 0;
    std::memcpy(&v, cell, sizeof(v));
    s_rendezvousCbResult.store(v);
}
static void test_blocking_native_callback_during_gc()
{
    TEST(blocking_native_callback_during_gc);
    CompiledModule mod = buildLinkedModule("k7_cb_gc",
        "import thread;\n"
        "native void nbReenter(int ms);\n"
        "class Node { public int v; }\n"
        "void churner() {\n"
        "    int i = 0;\n"
        "    while (i < 60000) { string g = \"g\" + i; i = i + 1; }\n"
        "}\n"
        "int cb_body() {\n"
        "    Node keep = Node();\n"
        "    keep.v = 55;\n"
        "    int i = 0;\n"
        "    while (i < 20000) { string s = \"c\" + i; i = i + 1; }\n"
        "    return keep.v;\n"
        "}\n"
        "int main() {\n"
        "    Thread t = thread.spawn(churner);\n"
        "    nbReenter(100);\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(2);
    exec.SetGcStressThresholds(4);
    s_rendezvousCbExec = &exec;
    s_rendezvousCbResult.store(-1);
    exec.RegisterNativeBlocking("k7_cb_gc.nbReenter", &NbReenterChurn);
    CHECK(exec.Execute(mod) == 0,
          "run completes while a blocking-native callback re-enters "
          "bytecode under a GC storm");
    CHECK(s_rendezvousCbResult.load() == 55,
          "callback held its value across every collection (got "
          + std::to_string(s_rendezvousCbResult.load()) + ")");
    s_rendezvousCbExec = nullptr;
    PASS();
}

//宿主最外层轮询门（参与根深度）：宿主线程（非载体）在 run 在飞期间
//经 CallByName 进入长分配循环——桥把它注册并翻开执行位；其嵌套深度
//恒 ≥1（桥的 NestEvalGuard 无条件递增），裸「深度>0 抑制」会让它整
//跑永不轮询，任何把它计入分母的停止发起都等不到到达 → 死锁挂死。
//断言：不挂死、宿主侧结果正确。
static std::atomic<int> s_hostPhase{0};    //0=未开始 1=宿主在飞 2=完成
static std::atomic<int> s_hostLoopResult{-1};
static void HostGo(NativeHost*, uint8_t*, const uint8_t*, int)
{
    s_hostPhase.store(1);
}
static void HostPhase(NativeHost*, uint8_t* ret, const uint8_t*, int)
{
    native::ReturnInt(ret, s_hostPhase.load());
}
static void test_host_outermost_polls()
{
    TEST(host_outermost_polls);
    CompiledModule mod = buildLinkedModule("k7_host_poll",
        "import thread;\n"
        "native void hostGo();\n"
        "native int hostPhase();\n"
        "class Node { public int v; }\n"
        "void churner() {\n"
        "    int i = 0;\n"
        "    while (i < 40000) { string g = \"g\" + i; i = i + 1; }\n"
        "}\n"
        "int host_loop() {\n"
        "    Node keep = Node();\n"
        "    keep.v = 99;\n"
        "    int i = 0;\n"
        "    while (i < 30000) { string s = \"h\" + i; i = i + 1; }\n"
        "    return keep.v;\n"
        "}\n"
        "int main() {\n"
        "    hostGo();\n"
        "    Thread t1 = thread.spawn(churner);\n"
        "    Thread t2 = thread.spawn(churner);\n"
        "    while (hostPhase() != 2) { thread.yield(); }\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    return 0;\n"
        "}\n");
    VmExecutor exec;
    exec.SetCarrierThreadCount(2);
    exec.SetGcStressThresholds(4);
    exec.RegisterNative("k7_host_poll.hostGo", &HostGo);
    exec.RegisterNative("k7_host_poll.hostPhase", &HostPhase);
    s_hostPhase.store(0);
    s_hostLoopResult.store(-1);
    std::thread host([&] {
        while (s_hostPhase.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        uint8_t cell[8] = {};
        int32_t v = -2;   //bridge failure sentinel — never deadlock main
        try {
            exec.CallByName("k7_host_poll.host_loop", nullptr, 0, cell);
            std::memcpy(&v, cell, sizeof(v));
        } catch (const std::exception&) {
        }
        s_hostLoopResult.store(v);
        s_hostPhase.store(2);
    });
    CHECK(exec.Execute(mod) == 0,
          "run completes while the host thread executes its own "
          "bytecode");
    host.join();
    CHECK(s_hostLoopResult.load() == 99,
          "host outermost loop survived every collection (got "
          + std::to_string(s_hostLoopResult.load()) + ")");
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
        test_concurrent_global_init();
        test_executor_reuse_two_runs();
        test_single_carrier_deterministic_multi_unspecified();
        test_carrier_pool_two_runs();
        test_aux_carriers_actually_execute();
        test_gil_wait_counters_whitebox();
        test_gil_wait_time_counters();
        test_gil_wait_counters_no_contention();
        test_debug_focus_exclusive_dispatch();
        test_debug_focus_held_wait();
        test_held_context_mirror_reset_after_run();
        test_affinity_prefers_last_runner();
        test_affinity_window_fallback();
        test_affinity_single_carrier_fifo_identity();
        test_timer_wake_under_multi_carrier();
        test_aux_carrier_error_confluence();
        runGuarded(test_blocking_native_releases_gil);
        runGuarded(test_blocking_native_staged_roots_survive_gc_storm);
        runGuarded(test_native_root_owner_removal);
        runGuarded(test_zombie_carrier_generation_guard);
        runGuarded(test_executing_bit_off_while_native_blocked);
        runGuarded(test_gc_collection_count_under_stress);
        runGuarded(test_executing_bit_intact_after_stop_path);
        runGuarded(test_nested_window_single_membership);
        runGuarded(test_backoff_heap_after_cliff);
        runGuarded(test_backoff_string_symmetric);
        runGuarded(test_stress_keeps_high_cadence);
        runGuarded(test_thresholds_persist_across_runs);
        runGuarded(test_reset_gc_thresholds_restores_defaults);
        runGuarded(test_shutdown_signal_passthrough);
        runGuarded(test_gc_bitmap_matches_conservative_band);
        runGuarded(test_gc_bitmap_survives_ncu_roundtrip);
        runGuarded(test_real_multiparty_rendezvous);
        runGuarded(test_single_carrier_inline_shape);
        runGuarded(test_blocking_native_callback_during_gc);
        runGuarded(test_host_outermost_polls);
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
        test_scheduler_quota_skips_single_context();
        test_scheduler_default_quota_single_context();
        test_scheduler_registry_lifecycle();
        test_scheduler_quota_skips_nested_windows();
        test_thread_ns_id_main();
        test_thread_ns_yield_single_context();
        test_thread_ns_yield_in_tostring_rejected();
        test_thread_ns_unknown_member();
        test_thread_ns_spawn_arg_not_func();
        test_thread_ns_spawn_arg_nonvoid();
        test_thread_ns_spawn_arg_with_params();
        test_thread_ns_spawn_compile_ok();
        test_thread_mutex_channel_compile_ok();
        test_thread_package_import();
        test_thread_package_name_duplicate();
        test_thread_requires_import();
        test_typed_spawn_requires_import();
        test_thread_ns_user_name_shadows();
        test_thread_ns_cross_module();
        runGuarded(test_spawn_join_runs_worker);
        runGuarded(test_spawn_round_robin_under_quota);
        runGuarded(test_yield_park_rearms_fresh_quantum);
        runGuarded(test_join_idempotent_after_finish);
        runGuarded(test_join_before_target_runs);
        runGuarded(test_join_failed_target_rethrows_original);
        runGuarded(test_spawn_bound_method_root);
        runGuarded(test_spawn_via_delegate_variable);
        runGuarded(test_worker_thread_id_nonzero);
        runGuarded(test_main_return_drops_unfinished_worker);
        runGuarded(test_gc_keeps_parked_context_roots);
        runGuarded(test_sleep_zero_yields);
        runGuarded(test_sleep_wakes_by_deadline);
        runGuarded(test_sleep_rejects_negative_duration);
        runGuarded(test_thread_ns_sleep_join_in_tostring_rejected);
        runGuarded(test_mutex_serializes_critical_section);
        runGuarded(test_mutex_wakes_waiters_fifo);
        runGuarded(test_mutex_trylock_reports_held_state);
        runGuarded(test_gc_keeps_mutex_parked_roots);
        runGuarded(test_mutex_unlock_without_hold_rejected);
        runGuarded(test_mutex_relock_deadlock_surfaces);
        runGuarded(test_channel_fifo_buffer_order);
        runGuarded(test_channel_recv_parks_until_send);
        runGuarded(test_channel_closed_semantics);
        runGuarded(test_channel_closed_drain_fifo);
        runGuarded(test_channel_close_drains_then_closed_throws);
        runGuarded(test_channel_send_after_close_catchable);
        runGuarded(test_channel_close_wakes_parked_recv_with_throw);
        runGuarded(test_channel_typed_roundtrip);
        runGuarded(test_channel_buffered_survives_gc_storm);
        runGuarded(test_channel_bare_name_rejected);
        runGuarded(test_channel_nonempty_init_rejected);
        runGuarded(test_channel_recv_in_tostring_rejected);
        runGuarded(test_park_in_try_throw_after_resume_caught);
        runGuarded(test_worker_recv_park_throw_cross_frame_caught);
        runGuarded(test_finally_runs_after_timer_park);
        runGuarded(test_worker_finally_after_recv_park);
        runGuarded(test_park_inside_catch_body);
        runGuarded(test_mixed_storm_channels_mutex_gc);
        runGuarded(test_spawn_future_typed_and_isdone);
        runGuarded(test_spawn_future_result_survives_gc);
        runGuarded(test_future_get_value_and_park);
        runGuarded(test_future_get_string_result_survives_gc);
        runGuarded(test_future_get_multi_waiter);
        runGuarded(test_future_get_rethrows_target_failure);
        runGuarded(test_channel_bounded_ctor_capacity);
        runGuarded(test_channel_bounded_send_parks_fifo);
        runGuarded(test_channel_bounded_close_wakes_sender);
        runGuarded(test_channel_capacity_zero_unbounded);
        runGuarded(test_channel_default_ctor_smallest_call_shape);
        runGuarded(test_channel_negative_capacity_throws);
        runGuarded(test_channel_capacity_non_int_rejected);
        runGuarded(test_channel_bare_ctor_no_args_rejected);
        runGuarded(test_channel_bounded_pending_survives_gc);
        runGuarded(test_eval_stack_routes_to_fallback_slot);
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
