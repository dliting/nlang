// test_work_pool.cpp — VmWorkPool unit tests (r1补注4: generic native
// worker pool, the concurrent library's native engine layer).
#include "VmWorkPool.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using namespace nlang;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

//1) submit runs fn on a worker; Shutdown joins every in-flight worker.
static void TestRunsAndJoins()
{
    VmWorkPool pool;
    std::atomic<int> counter{0};
    for (int i = 0; i < 8; ++i) {
        pool.Submit([](void* arg) {
            (*static_cast<std::atomic<int>*>(arg)).fetch_add(1);
        }, &counter, -1, 0);
    }
    pool.Shutdown();                       //joins all workers
    CHECK(counter.load() == 8);
}

//2) priority: one worker, a blocker occupies it while a normal and a
//   high job queue up; after release the high job runs first.
//   The pool takes plain C function pointers (the ABI submitWork
//   contract), so the jobs thread their state through the arg
//   parameter — capturing lambdas would not convert.
static void TestPriorityOrder()
{
    VmWorkPool pool(1);                    //single worker for determinism
    std::atomic<bool> releaseBlocker{false};
    struct OrderLog {
        std::mutex m;
        std::string s;
    } log;
    pool.Submit([](void* arg) {
        auto* flag = static_cast<std::atomic<bool>*>(arg);
        while (!flag->load()) { std::this_thread::yield(); }
    }, &releaseBlocker, -1, 0);
    pool.Submit([](void* arg) {
        auto* lg = static_cast<OrderLog*>(arg);
        std::lock_guard<std::mutex> lk(lg->m);
        lg->s.push_back('N');
    }, &log, -1, 0);
    pool.Submit([](void* arg) {
        auto* lg = static_cast<OrderLog*>(arg);
        std::lock_guard<std::mutex> lk(lg->m);
        lg->s.push_back('H');
    }, &log, -1, 1);
    releaseBlocker.store(true);
    pool.Shutdown();                       //drains the queue, then joins
    CHECK(log.s == "HN");
}

//3) lazy start: no worker before the first submit; workers exist after.
//   (The ack byte itself rides a caller-owned socket fd — exercised by
//   task 7's net_jobs integration, not by this no-ack unit test.)
static void TestLazyStart()
{
    VmWorkPool pool;
    CHECK(pool.Running() == 0);
    std::atomic<bool> ran{false};
    pool.Submit([](void* arg) {
        *static_cast<std::atomic<bool>*>(arg) = true;
    }, &ran, -1, 0);
    CHECK(pool.Running() > 0);
    pool.Shutdown();
    CHECK(ran.load());
}

int main()
{
    TestRunsAndJoins();
    TestPriorityOrder();
    TestLazyStart();
    if (g_failures > 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
