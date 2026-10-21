/*---
    test_stdlib_concurrency.cpp — K3 Task 5：stdlib/concurrent.n（纯 NLang
    WaitGroup/Semaphore）行为测试。真实编译＋真实执行（同 test_threads.cpp
    harness 形态）：import concurrent 经 stdlib 目录解析，断言只用顺序与
    终值（无墙钟）。
---*/
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
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
        / "nlang_test_stdlib_concurrency";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + link + execute one source (test_threads.cpp harness shape);
//returns main()'s value, throws std::runtime_error with diagnostics on
//build failure — a failure here is a test bug, not behavior under test.
static int runSourceCapturing(const std::string& tag,
                              const std::string& source, std::string& rOut)
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
    if (!builder.BuildArtifacts() || !std::filesystem::exists(modPath)) {
        std::string diag;
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            diag += (*it)->Message() + "\n";
        throw std::runtime_error("build failed:\n" + diag);
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    CompiledModule mod =
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey);

    class CaptureIo : public IHostIo {
    public:
        void OnOutput(std::string_view text) override { m_out += text; }
        std::string m_out;
    };
    VmExecutor exec;
    CaptureIo io;
    exec.SetHostIo(&io);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
}

//WaitGroup：add(3) 后三个 worker 各写一字再 done()；main wait() 必须等
//全部 done 才继续——输出恰为 "xxxD"（单载体 FIFO 下全序确定）。杀
//"wait 不阻塞"突变（D 会抢在某个 x 前）与"done 不投递"突变（死锁守卫）。
void test_waitgroup_releases_waiters()
{
    TEST(waitgroup_releases_waiters);
    std::string out;
    const int rc = runSourceCapturing("wg_wait",
        "import thread;\n"
        "import concurrent;\n"
        "import io;\n"
        "class Worker {\n"
        "    public WaitGroup wg;\n"
        "    public void work() {\n"
        "        io.write(\"x\");\n"
        "        wg.done();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    WaitGroup wg = WaitGroup();\n"
        "    Worker w = Worker();\n"
        "    w.wg = wg;\n"
        "    wg.add(3);\n"
        "    Thread a = thread.spawn(w.work);\n"
        "    Thread b = thread.spawn(w.work);\n"
        "    Thread c = thread.spawn(w.work);\n"
        "    wg.wait();\n"
        "    io.write(\"D\");\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "waitgroup program completes (rc=" + std::to_string(rc) + ")");
    CHECK(out == "xxxD", "wait() holds main until every done() "
          "(got: " + out + ")");
    PASS();
}

//WaitGroup 计数语义：add 累加两个批次共 3，全部 done 后 wait 立即通过
//（无残留停泊——残留会撞死锁守卫）。
void test_waitgroup_add_accumulates()
{
    TEST(waitgroup_add_accumulates);
    std::string out;
    const int rc = runSourceCapturing("wg_add",
        "import thread;\n"
        "import concurrent;\n"
        "import io;\n"
        "class Worker {\n"
        "    public WaitGroup wg;\n"
        "    public void work() {\n"
        "        wg.done();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    WaitGroup wg = WaitGroup();\n"
        "    Worker w = Worker();\n"
        "    w.wg = wg;\n"
        "    wg.add(1);\n"
        "    wg.add(2);\n"
        "    Thread a = thread.spawn(w.work);\n"
        "    Thread b = thread.spawn(w.work);\n"
        "    Thread c = thread.spawn(w.work);\n"
        "    wg.wait();\n"
        "    io.write(\"ok\");\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "accumulated adds all satisfied (rc=" + std::to_string(rc) + ")");
    CHECK(out == "ok", "wait passes after the summed batches");
    PASS();
}

//Semaphore(2)：4 个 worker acquire-记峰-release；共享 inside/peak 经 Mutex
//保护，join 提供断言的 happens-before。峰值恰为 2（许可确实限量）且不超 2
//（acquire 真正扣减）。yield 拉宽重叠窗口使峰值可达上限。
void test_semaphore_limits_concurrency()
{
    TEST(semaphore_limits_concurrency);
    std::string out;
    const int rc = runSourceCapturing("sem_limit",
        "import thread;\n"
        "import concurrent;\n"
        "import io;\n"
        "class Runner {\n"
        "    public Semaphore sem;\n"
        "    public Mutex gate;\n"
        "    public int inside;\n"
        "    public int peak;\n"
        "    public void work() {\n"
        "        sem.acquire();\n"
        "        gate.lock();\n"
        "        inside = inside + 1;\n"
        "        if (inside > peak) {\n"
        "            peak = inside;\n"
        "        }\n"
        "        gate.unlock();\n"
        "        thread.yield();\n"
        "        gate.lock();\n"
        "        inside = inside - 1;\n"
        "        gate.unlock();\n"
        "        sem.release();\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Runner r = Runner();\n"
        "    r.sem = Semaphore(2);\n"
        "    r.gate = Mutex();\n"
        "    Thread a = thread.spawn(r.work);\n"
        "    Thread b = thread.spawn(r.work);\n"
        "    Thread c = thread.spawn(r.work);\n"
        "    Thread d = thread.spawn(r.work);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(c);\n"
        "    thread.join(d);\n"
        "    io.write(r.peak.toString());\n"
        "    if (r.peak == 2) {\n"
        "        return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n",
        out);
    CHECK(rc == 0, "semaphore program completes (rc=" + std::to_string(rc) + ")");
    CHECK(out == "2", "peak concurrency equals the permit count "
          "(got: " + out + ")");
    PASS();
}

//Catch per-test exceptions (build failures, deadlock-guard throws) as
//FAIL lines instead of killing the whole binary (test_threads.cpp
//runGuarded shape).
static void runGuarded(void (*fn)())
{
    try {
        fn();
    } catch (const std::exception& e) {
        FAIL(std::string("exception: ") + e.what());
    }
}

int main()
{
    //In-process ModuleBuilder host: the interned IdString tables must
    //exist before the first Build (omitting this segfaults inside
    //Build — uncatchable), and the compiler-side cast table must be
    //anchored too — without it every assignment/return verdict is
    //TCK_None and the whole compilation drowns in "Incompatible type"
    //diagnostics (test_stdlib.cpp same shape).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    runGuarded(test_waitgroup_releases_waiters);
    runGuarded(test_waitgroup_add_accumulates);
    runGuarded(test_semaphore_limits_concurrency);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
