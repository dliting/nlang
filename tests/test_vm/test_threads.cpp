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
#include <list>
#include <stdexcept>
#include <string>

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
//到期都走了「跳过」臂（skip 计数与 fire 计数全等），程序结果精确。
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
//单上下文下同样只走跳过臂——钉死常量在真实执行路径上生效。
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
    //失败路径：根帧异常逃逸走 catch(...) 臂 → MarkFailed。
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

//spawn 参数形态：返回值非 void 的函数拒绝。
void test_thread_ns_spawn_arg_nonvoid()
{
    TEST(thread_ns_spawn_arg_nonvoid);
    CHECK(buildFailsWith("thread_sp_nv",
        "int f() { return 1; }\n"
        "int main() {\n"
        "    thread.spawn(f);\n"
        "    return 0;\n"
        "}\n",
        "spawn"),
        "spawn of a non-void function is a compile error");
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
            "    Channel c = Channel();\n"
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
//装链接后执行——导入单元里的 OP_CallFunc 内建臂在链接产物上照常派发。
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
