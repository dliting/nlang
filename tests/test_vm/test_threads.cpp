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

//未知 thread.foo()：编译期诊断点名成员。
void test_thread_ns_unknown_member()
{
    TEST(thread_ns_unknown_member);
    CHECK(buildFailsWith("thread_unknown",
        "int main() {\n"
        "    thread.nope();\n"
        "    return 0;\n"
        "}\n",
        "thread.nope"),
        "unknown thread namespace member is a compile error naming it");
    PASS();
}

//spawn 参数形态：非函数值拒绝。
void test_thread_ns_spawn_arg_not_func()
{
    TEST(thread_ns_spawn_arg_not_func);
    CHECK(buildFailsWith("thread_sp_int",
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

//O1：包名 thread 保留 — 源文件 thread.n 派生模块路径 "thread"，编译期
//拒绝且报错指路内建命名空间。
void test_thread_package_name_reserved()
{
    TEST(thread_package_name_reserved);
    CHECK(buildFailsWith("thread",
        "int main() { return 0; }\n",
        "reserved"),
        "a package named 'thread' is rejected with a pointing message");
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
        "import io;\n"
        "class Boom : Exception {\n"
        "    public int Boom(string m) {\n"
        "        super(m);\n"
        "        return 0;\n"
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
        "import io;\n"
        "class Worker {\n"
        "    int tag;\n"
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
        "import io;\n"
        "struct Cell {\n"
        "    int v;\n"
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
        "import io;\n"
        "struct Cell {\n"
        "    int v;\n"
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
        "    int v;\n"
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
        "import io;\n"
        "struct Cell {\n"
        "    int v;\n"
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
        "class Boom : Exception {\n"
        "    public int Boom(string m) {\n"
        "        super(m);\n"
        "        return 0;\n"
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

//K2 INV-3：FIFO 队列＋GIL＋「取队与执行同一持有窗口」⇒ 上下文的执行
//窗口序只由出队序决定，与载体数无关。同一程序在 1 载体与 4 载体下输出
//必须逐字节一致；4 载体重复运行若干次以覆盖窗口竞争的唤醒顺序差异。
static void test_carrier_count_deterministic_parity()
{
    TEST(carrier_count_deterministic_parity);
    const char* src =
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
    for (int round = 0; round < 5; ++round) {
        std::string out4;
        const int rc4 = runWithCarriers("carrier_parity_4", src, 4, out4);
        CHECK(rc4 == 0, "four carriers complete the program");
        CHECK(out4 == out1,
              "INV-3: output is byte-identical across carrier counts (got: "
              + out4 + ", want: " + out1 + ")");
    }
    PASS();
}

//池生命周期：同一执行器（池已创建过线程）连续两次 Execute——辅助线程
//按 run 启停、调度器状态每 run 复位，两次结果各自正确且无跨 run 残留。
static void test_carrier_pool_two_runs()
{
    TEST(carrier_pool_two_runs);
    const char* src =
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

//辅助载体异常汇合（Review Focus #3）：死锁检测在最后一个即将空闲的
//载体上触发——可能是主载体（直接传播）也可能是辅助载体（异常进汇合
//槽，主载体在汇合点唯一重抛）。无论哪条路径，Execute 抛出的错误与消
//息必须一致；多轮重复以覆盖两条路径。
static void test_aux_carrier_error_confluence()
{
    TEST(aux_carrier_error_confluence);
    const char* src =
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

//协议判别器 1（GIL 释放）：main 阻塞 150ms，worker 在窗口内抢到 GIL 并
//输出。main 从 spawn 到阻塞调用之间无回边（不会被配额抢占），故 worker
//的输出先于 main-after 只能来自「阻塞期间 GIL 离手」；不释放则顺序反转
//（worker 只能在 main 返回后运行）——测试变红。
static void test_blocking_native_releases_gil()
{
    TEST(blocking_native_releases_gil);
    std::string out;
    const int rc = runBlockingNative("bn_gil",
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

//协议判别器 3（停机穿透）：worker 在阻塞原生内时 main 返回（停机请求）
//——原生返回后必须经 ShutdownSignal 丢弃该上下文：无输出、无结果消费、
//帧留 ctx、INV-2 成立。缺停机检查 ⇒ worker 复活并输出 after-native——
//测试变红。main 先阻塞 10ms 保证 worker 确实进入原生（判别前提成立）。
static void test_shutdown_signal_passthrough()
{
    TEST(shutdown_signal_passthrough);
    std::string out;
    const int rc = runBlockingNative("bn_stop",
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
    "    public int Box(int x) { v = x; return 0; }\n"
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
        test_carrier_count_deterministic_parity();
        test_carrier_pool_two_runs();
        test_aux_carriers_actually_execute();
        test_timer_wake_under_multi_carrier();
        test_aux_carrier_error_confluence();
        runGuarded(test_blocking_native_releases_gil);
        runGuarded(test_blocking_native_staged_roots_survive_gc_storm);
        runGuarded(test_shutdown_signal_passthrough);
        runGuarded(test_gc_bitmap_matches_conservative_band);
        runGuarded(test_gc_bitmap_survives_ncu_roundtrip);
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
        test_thread_package_name_reserved();
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
