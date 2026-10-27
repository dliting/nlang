/*---
    test_stdlib_concurrency.cpp — stdlib/concurrent.n（WaitGroup）与内建
    Semaphore 句柄类（P2-1）行为测试。真实编译＋真实执行（同
    test_threads.cpp harness 形态）：stdlib 单元经 stdlib 目录解析，内建
    类无需 import；断言只用顺序与终值（无墙钟）。
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

//Compile + link + execute one source with an explicit carrier count
//(test_threads.cpp harness shape); returns main()'s value, throws
//std::runtime_error with diagnostics on build failure — a failure here
//is a test bug, not behavior under test. The carrier count must be set
//before Execute (the default is single-carrier).
static int runSourceCapturingCarriers(const std::string& tag,
                                      const std::string& source,
                                      uint32_t carriers, std::string& rOut)
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
    exec.SetCarrierThreadCount(carriers);
    const int rc = exec.Execute(mod);
    rOut = io.m_out;
    return rc;
}

//Single-carrier default (the plain harness shape).
static int runSourceCapturing(const std::string& tag,
                              const std::string& source, std::string& rOut)
{
    return runSourceCapturingCarriers(tag, source, 1, rOut);
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

//Native Semaphore(2)：4 个 worker acquire-记峰-release；共享 inside/peak
//经 Mutex 保护，join 提供断言的 happens-before。峰值恰为 2（许可确实限
//量）且不超 2（acquire 真正扣减）。yield 拉宽重叠窗口使峰值可达上限。
void test_semaphore_limits_concurrency()
{
    TEST(semaphore_limits_concurrency);
    std::string out;
    const int rc = runSourceCapturing("sem_limit",
        "import thread;\n"
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

//Native Semaphore construction: the builtin needs no import (the Mutex
//shape) and accepts zero and positive initial counts.
void test_semaphore_native_constructs()
{
    TEST(semaphore_native_constructs);
    std::string out;
    const int rc = runSourceCapturing("sem_ctor",
        "import io;\n"
        "int main() {\n"
        "    Semaphore a = Semaphore(2);\n"
        "    Semaphore b = Semaphore(0);\n"
        "    io.write(\"ok\");\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "native ctor accepts zero and positive initial counts "
          "(rc=" + std::to_string(rc) + ")");
    CHECK(out == "ok", "construction completes");
    PASS();
}

//Negative initial count raises the Exception base with a message naming
//the rule (the Channel capacity-error shape; the caller-catch assert is
//the mutex_unlock_without_hold_rejected form in test_threads.cpp).
void test_semaphore_negative_initial_raises()
{
    TEST(semaphore_negative_initial_raises);
    bool threw = false;
    std::string what;
    try {
        std::string out;
        runSourceCapturing("sem_ctor_neg",
            "int main() {\n"
            "    Semaphore s = Semaphore(-1);\n"
            "    return 0;\n"
            "}\n",
            out);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "negative initial count must raise");
    CHECK(what.find("semaphore initial permit count must be at least 0")
          != std::string::npos,
          "message names the rule (got: " + what + ")");
    PASS();
}

//Zero-permit acquire parks and release wakes: the worker cannot pass
//acquire until main releases. Completion itself is the assertion — a
//non-parking acquire spins forever and hits the deadlock guard, a lost
//wakeup the same.
void test_semaphore_park_and_wake()
{
    TEST(semaphore_park_and_wake);
    std::string out;
    const int rc = runSourceCapturing("sem_park",
        "import thread;\n"
        "import io;\n"
        "class Worker {\n"
        "    public Semaphore sem;\n"
        "    public void go() {\n"
        "        sem.acquire();\n"
        "        io.write(\"A\");\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(0);\n"
        "    Worker w = Worker();\n"
        "    w.sem = s;\n"
        "    Thread t = thread.spawn(w.go);\n"
        "    thread.sleep(5);\n"
        "    s.release();\n"
        "    io.write(\"R\");\n"
        "    thread.join(t);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "park/wake program completes (rc="
          + std::to_string(rc) + ")");
    CHECK(out.find("A") != std::string::npos
          && out.find("R") != std::string::npos,
          "worker passes acquire after release (got: " + out + ")");
    PASS();
}

//FIFO fairness: three workers park in arrival order (sleeps separate
//the spawns deterministically on the single carrier); three releases
//wake them in the same order — the wake sequence must equal the
//arrival sequence.
void test_semaphore_fifo_wake_order()
{
    TEST(semaphore_fifo_wake_order);
    std::string out;
    const int rc = runSourceCapturing("sem_fifo",
        "import thread;\n"
        "import io;\n"
        "class Worker {\n"
        "    public Semaphore sem;\n"
        "    public string tag;\n"
        "    public void go() {\n"
        "        sem.acquire();\n"
        "        io.write(tag);\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(0);\n"
        "    Worker a = Worker();\n"
        "    a.sem = s;\n"
        "    a.tag = \"A\";\n"
        "    Worker b = Worker();\n"
        "    b.sem = s;\n"
        "    b.tag = \"B\";\n"
        "    Worker c = Worker();\n"
        "    c.sem = s;\n"
        "    c.tag = \"C\";\n"
        "    Thread ta = thread.spawn(a.go);\n"
        "    thread.sleep(5);\n"
        "    Thread tb = thread.spawn(b.go);\n"
        "    thread.sleep(5);\n"
        "    Thread tc = thread.spawn(c.go);\n"
        "    thread.sleep(5);\n"
        "    s.release();\n"
        "    s.release();\n"
        "    s.release();\n"
        "    thread.join(ta);\n"
        "    thread.join(tb);\n"
        "    thread.join(tc);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "fifo program completes (rc=" + std::to_string(rc) + ")");
    CHECK(out == "ABC", "wake order equals arrival order (got: " + out + ")");
    PASS();
}

//tryAcquire both arms: a permit present → true and taken (the second
//probe sees zero); after a release the probe succeeds again.
void test_semaphore_try_acquire_arms()
{
    TEST(semaphore_try_acquire_arms);
    std::string out;
    const int rc = runSourceCapturing("sem_try",
        "import io;\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(1);\n"
        "    if (s.tryAcquire()) { io.write(\"1\"); }\n"
        "    if (!s.tryAcquire()) { io.write(\"0\"); }\n"
        "    s.release();\n"
        "    if (s.tryAcquire()) { io.write(\"2\"); }\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "tryAcquire program completes (rc="
          + std::to_string(rc) + ")");
    CHECK(out == "102", "take when available, miss at zero, take again "
          "after release (got: " + out + ")");
    PASS();
}

//Over-release is legal (no ownership precondition — the Java/Python
//convention): two releases with no waiter bank two permits, two
//acquires drain them, the third probe misses.
void test_semaphore_overrelease_then_acquire()
{
    TEST(semaphore_overrelease_then_acquire);
    std::string out;
    const int rc = runSourceCapturing("sem_over",
        "import io;\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(0);\n"
        "    s.release();\n"
        "    s.release();\n"
        "    if (s.tryAcquire()) { io.write(\"a\"); }\n"
        "    if (s.tryAcquire()) { io.write(\"b\"); }\n"
        "    if (!s.tryAcquire()) { io.write(\"c\"); }\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "over-release program completes (rc="
          + std::to_string(rc) + ")");
    CHECK(out == "abc", "released permits stay countable (got: " + out + ")");
    PASS();
}

//Zero-permit acquire inside a user toString must be rejected as a
//suspension inside a nested evaluation: io.write of an array coerces
//the elements through toString (the nested window; the direct class
//argument shape in test_threads.cpp). The caller-catch assert mirrors
//mutex_unlock_without_hold_rejected there.
void test_semaphore_acquire_in_tostring_rejects()
{
    TEST(semaphore_acquire_in_tostring_rejects);
    bool threw = false;
    std::string what;
    try {
        std::string out;
        runSourceCapturing("sem_tostring",
            "import io;\n"
            "class C {\n"
            "    public string toString() {\n"
            "        Semaphore s = Semaphore(0);\n"
            "        s.acquire();\n"
            "        return \"x\";\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    C c = C();\n"
            "    C[] arr = [c];\n"
            "    io.write(arr);\n"
            "    return 0;\n"
            "}\n",
            out);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "zero-permit acquire in toString must raise");
    CHECK(what.find("cannot suspend") != std::string::npos,
          "message names the nested-window rule (got: " + what + ")");
    PASS();
}

//Permit conservation across four carriers: three acquirers, six
//releasers — after every join the banked count is exactly releases
//minus acquires; the probe pattern pins it (no permit lost or
//duplicated by the racing handoff/count paths).
void test_semaphore_permits_conserved_multicarrier()
{
    TEST(semaphore_permits_conserved_multicarrier);
    std::string out;
    const int rc = runSourceCapturingCarriers("sem_conservation",
        "import thread;\n"
        "import io;\n"
        "class Releaser {\n"
        "    public Semaphore sem;\n"
        "    public void go() { sem.release(); }\n"
        "}\n"
        "class Acquirer {\n"
        "    public Semaphore sem;\n"
        "    public void go() { sem.acquire(); }\n"
        "}\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(0);\n"
        "    Releaser r = Releaser();\n"
        "    r.sem = s;\n"
        "    Acquirer a = Acquirer();\n"
        "    a.sem = s;\n"
        "    Thread t1 = thread.spawn(a.go);\n"
        "    Thread t2 = thread.spawn(a.go);\n"
        "    Thread t3 = thread.spawn(a.go);\n"
        "    Thread t4 = thread.spawn(r.go);\n"
        "    Thread t5 = thread.spawn(r.go);\n"
        "    Thread t6 = thread.spawn(r.go);\n"
        "    Thread t7 = thread.spawn(r.go);\n"
        "    Thread t8 = thread.spawn(r.go);\n"
        "    Thread t9 = thread.spawn(r.go);\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    thread.join(t3);\n"
        "    thread.join(t4);\n"
        "    thread.join(t5);\n"
        "    thread.join(t6);\n"
        "    thread.join(t7);\n"
        "    thread.join(t8);\n"
        "    thread.join(t9);\n"
        "    if (s.tryAcquire()) { io.write(\"1\"); }\n"
        "    if (s.tryAcquire()) { io.write(\"1\"); }\n"
        "    if (s.tryAcquire()) { io.write(\"1\"); }\n"
        "    if (!s.tryAcquire()) { io.write(\"0\"); }\n"
        "    return 0;\n"
        "}\n",
        4, out);
    CHECK(rc == 0, "conservation program completes (rc="
          + std::to_string(rc) + ")");
    CHECK(out == "1110", "6 releases - 3 acquires leaves exactly 3 "
          "permits (got: " + out + ")");
    PASS();
}

//A parked acquirer's frame is a GC root across the park window: the
//worker holds a heap-referenced local across a zero-permit acquire
//while two churners drive natural-threshold collections (no --gc-stress
//— it suppresses the race window), then release wakes it and the local
//must read intact (concurrent_channel_park_gcwindow.n shape).
void test_semaphore_park_gc_window()
{
    TEST(semaphore_park_gc_window);
    std::string out;
    const int rc = runSourceCapturing("sem_gcwindow",
        "import thread;\n"
        "import io;\n"
        "class Churner {\n"
        "    public int spins;\n"
        "    public void churn() {\n"
        "        int i = 0;\n"
        "        string acc = \"s\";\n"
        "        while (i < spins) {\n"
        "            acc = acc + \"x\" + i.toString();\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "class Waiter {\n"
        "    public Semaphore sem;\n"
        "    public void go() {\n"
        "        string token = \"secret\";\n"
        "        sem.acquire();\n"
        "        if (token == \"secret\") {\n"
        "            io.write(\"ok\");\n"
        "        }\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Semaphore s = Semaphore(0);\n"
        "    Churner c1 = Churner();\n"
        "    c1.spins = 1500;\n"
        "    Churner c2 = Churner();\n"
        "    c2.spins = 1500;\n"
        "    Waiter w = Waiter();\n"
        "    w.sem = s;\n"
        "    Thread t1 = thread.spawn(c1.churn);\n"
        "    Thread t2 = thread.spawn(c2.churn);\n"
        "    Thread t3 = thread.spawn(w.go);\n"
        "    thread.sleep(5);\n"
        "    s.release();\n"
        "    thread.join(t1);\n"
        "    thread.join(t2);\n"
        "    thread.join(t3);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "gc-window program completes (rc="
          + std::to_string(rc) + ")");
    CHECK(out == "ok", "parked local survives collections across the "
          "window (got: " + out + ")");
    PASS();
}

// --- 0.10.22: AtomicInt — lock-free single-value primitives ---

//Default construction lands at 0 (the default-parameter staging path),
//the explicit-initial constructor takes, and store/load round-trip.
void test_atomic_ctor_load_store()
{
    TEST(atomic_ctor_load_store);
    std::string out;
    const int rc = runSourceCapturing("atomic_basic",
        "import io;\n"
        "int main() {\n"
        "    AtomicInt a = AtomicInt();\n"
        "    if (a.load() != 0) return 1;\n"
        "    AtomicInt b = AtomicInt(5);\n"
        "    if (b.load() != 5) return 2;\n"
        "    b.store(9);\n"
        "    io.write(b.load());\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "ctor/load/store program completes");
    CHECK(out == "9", "store then load round-trips (got: " + out + ")");
    PASS();
}

//getAndSet exchanges and returns the PREVIOUS value (Java getAndSet).
void test_atomic_get_and_set_returns_old()
{
    TEST(atomic_get_and_set_returns_old);
    std::string out;
    const int rc = runSourceCapturing("atomic_getandset",
        "int main() {\n"
        "    AtomicInt a = AtomicInt(7);\n"
        "    if (a.getAndSet(9) != 7) return 1;\n"
        "    if (a.load() != 9) return 2;\n"
        "    return 7;\n"
        "}\n",
        out);
    CHECK(rc == 7, "getAndSet returned the old value (got rc " +
        std::to_string(rc) + ")");
    PASS();
}

//CAS gate both ways: a matching expect swaps and returns true; a
//missing expect leaves the value untouched and returns false.
void test_atomic_compare_exchange_gate()
{
    TEST(atomic_compare_exchange_gate);
    std::string out;
    const int rc = runSourceCapturing("atomic_cas",
        "int main() {\n"
        "    AtomicInt a = AtomicInt(5);\n"
        "    if (!a.compareExchange(5, 9)) return 1;\n"
        "    if (a.load() != 9) return 2;\n"
        "    if (a.compareExchange(5, 1)) return 3;\n"
        "    if (a.load() != 9) return 4;\n"
        "    return 7;\n"
        "}\n",
        out);
    CHECK(rc == 7, "compareExchange gated both directions (got rc " +
        std::to_string(rc) + ")");
    PASS();
}

//The Java naming convention: getAndX returns the PREVIOUS value,
//xAndGet returns the NEW one — pinned across the whole add family.
void test_atomic_add_family()
{
    TEST(atomic_add_family);
    std::string out;
    const int rc = runSourceCapturing("atomic_addfam",
        "int main() {\n"
        "    AtomicInt a = AtomicInt(10);\n"
        "    if (a.getAndAdd(3) != 10) return 1;\n"
        "    if (a.load() != 13) return 2;\n"
        "    if (a.addAndGet(3) != 16) return 3;\n"
        "    if (a.getAndIncrement() != 16) return 4;\n"
        "    if (a.load() != 17) return 5;\n"
        "    if (a.incrementAndGet() != 18) return 6;\n"
        "    return 7;\n"
        "}\n",
        out);
    CHECK(rc == 7, "add family old/new conventions pinned (got rc " +
        std::to_string(rc) + ")");
    PASS();
}

//Core race test: four tasks each addAndGet(1) 2500 times across four
//carriers, with yields forcing genuine interleaving — the count must
//land exactly at 10000 (a torn read-update-write would lose updates).
void test_atomic_counter_race()
{
    TEST(atomic_counter_race);
    std::string out;
    const int rc = runSourceCapturingCarriers("atomic_race",
        "import thread;\n"
        "import io;\n"
        "class Bumper {\n"
        "    public AtomicInt counter;\n"
        "    public void bump() {\n"
        "        int i = 0;\n"
        "        while (i < 2500) {\n"
        "            counter.addAndGet(1);\n"
        "            if (i % 500 == 0) thread.yield();\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Bumper b = Bumper();\n"
        "    b.counter = AtomicInt(0);\n"
        "    Thread a = thread.spawn(b.bump);\n"
        "    Thread c = thread.spawn(b.bump);\n"
        "    Thread d = thread.spawn(b.bump);\n"
        "    Thread e = thread.spawn(b.bump);\n"
        "    thread.join(a);\n"
        "    thread.join(c);\n"
        "    thread.join(d);\n"
        "    thread.join(e);\n"
        "    io.write(b.counter.load());\n"
        "    return 0;\n"
        "}\n",
        4, out);
    CHECK(rc == 0, "race program completes");
    CHECK(out == "10000",
        "four-way concurrent increments all land (got: " + out + ")");
    PASS();
}

//CAS-as-flag: four tasks race to flip 0 → 1; exactly ONE wins, the
//losers' retries all fail, and the winner count is pinned.
void test_atomic_flag_pattern()
{
    TEST(atomic_flag_pattern);
    std::string out;
    const int rc = runSourceCapturingCarriers("atomic_flag",
        "import thread;\n"
        "import io;\n"
        "class Claimer {\n"
        "    public AtomicInt flag;\n"
        "    public AtomicInt winners;\n"
        "    public void claim() {\n"
        "        if (flag.compareExchange(0, 1)) winners.addAndGet(1);\n"
        "        if (flag.compareExchange(0, 1)) winners.addAndGet(100);\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Claimer c = Claimer();\n"
        "    c.flag = AtomicInt(0);\n"
        "    c.winners = AtomicInt(0);\n"
        "    Thread a = thread.spawn(c.claim);\n"
        "    Thread b = thread.spawn(c.claim);\n"
        "    Thread d = thread.spawn(c.claim);\n"
        "    Thread e = thread.spawn(c.claim);\n"
        "    thread.join(a);\n"
        "    thread.join(b);\n"
        "    thread.join(d);\n"
        "    thread.join(e);\n"
        "    io.write(c.winners.load());\n"
        "    return 0;\n"
        "}\n",
        4, out);
    CHECK(rc == 0, "flag program completes");
    CHECK(out == "1",
        "exactly one CAS claim wins across four racers (got: " + out
        + ")");
    PASS();
}

//No park path: load/store inside an implicit-formatting toString (the
//array form opens the window) run legally — the tryAcquire precedent.
void test_atomic_nested_eval_legal()
{
    TEST(atomic_nested_eval_legal);
    std::string out;
    const int rc = runSourceCapturing("atomic_tostring",
        "import io;\n"
        "class Box {\n"
        "    public AtomicInt a;\n"
        "    public string toString() {\n"
        "        a.store(7);\n"
        "        if (a.load() != 7) return \"bad\";\n"
        "        return \"ok\";\n"
        "    }\n"
        "}\n"
        "int main() {\n"
        "    Box b = Box();\n"
        "    b.a = AtomicInt(0);\n"
        "    Box[] arr = [b];\n"
        "    io.write(arr);\n"
        "    return 0;\n"
        "}\n",
        out);
    CHECK(rc == 0, "nested-eval program completes");
    CHECK(out == "ok",
        "load/store are legal inside the window (got: " + out + ")");
    PASS();
}

//A bare allocation (never constructed) has no side-table slot — the
//uninitialized-instance rejection, same shape as the Mutex family.
void test_atomic_uninit_rejected()
{
    TEST(atomic_uninit_rejected);
    bool threw = false;
    std::string what;
    try {
        std::string out;
        runSourceCapturing("atomic_uninit",
            "int main() {\n"
            "    AtomicInt m;\n"
            "    m.load();\n"
            "    return 0;\n"
            "}\n",
            out);
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "a call on a never-constructed instance must be an error");
    CHECK(what.find("uninitialized instance") != std::string::npos,
        "message names the rule (got: " + what + ")");
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
    runGuarded(test_semaphore_native_constructs);
    runGuarded(test_semaphore_negative_initial_raises);
    runGuarded(test_semaphore_park_and_wake);
    runGuarded(test_semaphore_fifo_wake_order);
    runGuarded(test_semaphore_try_acquire_arms);
    runGuarded(test_semaphore_overrelease_then_acquire);
    runGuarded(test_semaphore_acquire_in_tostring_rejects);
    runGuarded(test_semaphore_permits_conserved_multicarrier);
    runGuarded(test_semaphore_park_gc_window);
    runGuarded(test_atomic_ctor_load_store);
    runGuarded(test_atomic_get_and_set_returns_old);
    runGuarded(test_atomic_compare_exchange_gate);
    runGuarded(test_atomic_add_family);
    runGuarded(test_atomic_counter_race);
    runGuarded(test_atomic_flag_pattern);
    runGuarded(test_atomic_nested_eval_legal);
    runGuarded(test_atomic_uninit_rejected);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
