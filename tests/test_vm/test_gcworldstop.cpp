/*---
    test_gcworldstop.cpp - K6 Task 1: GcWorldStop protocol white-box tests.

    Real threads drive the multi-party rendezvous directly (no GIL, no
    executor): election, superseding, completion-epoch comparison, and
    the evaluation-window depth/suspend accounting transitions.
---*/
#include "GcWorldStop.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

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

//Single requester with its window up is necessarily the last arriver
//(the K6 product-path degenerate shape).
static void test_single_requester_is_last()
{
    TEST(single_requester_is_last);
    GcWorldStop stop;
    GcWorldStop::ExecWindowGuard guard(stop);
    CHECK(stop.WindowThreadCount() == 1, "guard counted");
    const uint64_t epoch = stop.RequestWorldStop();
    CHECK(stop.Arrive(epoch), "sole window is the last arriver");
    stop.Complete(epoch);
    stop.WaitForCompletion(epoch);   // already complete — returns at once
    PASS();
}

//Three guarded threads, main arriving deterministically last: exactly
//one Arrive reports last, and it is the one that arrived third. The
//windows-up gate pins expected=3 BEFORE the request (request snapshots
//WindowThreadCount); all CHECKs run after join — a failed CHECK must
//count as FAIL, not std::terminate on a joinable thread.
static void test_three_arrivers_elect_exactly_one()
{
    TEST(three_arrivers_elect_exactly_one);
    GcWorldStop stop;
    std::mutex mux;
    std::condition_variable cv;
    bool go = false;
    std::atomic<int> lastCount{0};
    std::atomic<uint64_t> epochSeen{0};
    std::atomic<int> arrived{0};
    std::atomic<int> windowsUp{0};
    auto participant = [&] {
        GcWorldStop::ExecWindowGuard guard(stop);
        windowsUp.fetch_add(1);
        std::unique_lock<std::mutex> lk(mux);
        cv.wait(lk, [&] { return go; });
        lk.unlock();
        if (stop.Arrive(epochSeen.load()))
            lastCount.fetch_add(1);
        arrived.fetch_add(1);
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 2; ++i) workers.emplace_back(participant);
    GcWorldStop::ExecWindowGuard ownGuard(stop);
    while (windowsUp.load() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool threeCounted = stop.WindowThreadCount() == 3;
    {
        std::lock_guard<std::mutex> lk(mux);
        epochSeen.store(stop.RequestWorldStop());
        go = true;
    }
    cv.notify_all();
    //Both workers arrive first (bounded poll — no hang on a bug).
    while (arrived.load() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool workersNotElected = lastCount.load() == 0;
    const bool mainElected = stop.Arrive(epochSeen.load());
    stop.Complete(epochSeen.load());
    for (auto& w : workers) w.join();
    stop.WaitForCompletion(epochSeen.load());
    CHECK(threeCounted, "three windows counted");
    CHECK(workersNotElected, "neither worker was elected");
    CHECK(mainElected, "main, arriving third of three, is elected");
    PASS();
}

//A superseding request folds the old epoch: stale Arrive reports false,
//and completing the NEW epoch releases waiters of the old one
//(completedEpoch >= epoch comparison — never a zero reset). Single
//thread, ONE guard: same-thread nesting counts once, so expected=1 and
//the thread's own arrival completes the new epoch.
static void test_supersede_folds_old_epoch()
{
    TEST(supersede_folds_old_epoch);
    GcWorldStop stop;
    GcWorldStop::ExecWindowGuard window(stop);
    const uint64_t e1 = stop.RequestWorldStop();
    const uint64_t e2 = stop.RequestWorldStop();   //supersede folds e1
    CHECK(e2 == e1 + 1, "epoch advanced by one");
    CHECK(!stop.Arrive(e1), "stale-epoch arrival is refused");
    CHECK(stop.Arrive(e2), "own-window arrival completes the new epoch");
    stop.Complete(e2);
    stop.WaitForCompletion(e1);   //old-epoch waiter released by e2 >= e1
    CHECK(true, "old-epoch waiter released");
    stop.Reset();
    CHECK(stop.RequestWorldStop() == 1,
          "Reset restarts the epoch numbering");
    PASS();
}

//Depth-nesting counts a thread once; suspend/resume move the count at
//the 0<->1 transitions (the blocking-native park shape).
static void test_window_depth_and_suspend()
{
    TEST(window_depth_and_suspend);
    GcWorldStop stop;
    CHECK(stop.WindowThreadCount() == 0, "no windows yet");
    {
        GcWorldStop::ExecWindowGuard outer(stop);
        {
            GcWorldStop::ExecWindowGuard inner(stop);
            CHECK(stop.WindowThreadCount() == 1,
                  "nested guards count the thread once");
        }
        CHECK(stop.WindowThreadCount() == 1, "inner drop keeps depth>0");
        stop.SuspendEvalWindow();
        CHECK(stop.WindowThreadCount() == 0,
              "suspend releases the thread's membership");
        stop.ResumeEvalWindow();
        CHECK(stop.WindowThreadCount() == 1, "resume re-adds it");
    }
    CHECK(stop.WindowThreadCount() == 0, "guard exit drops to zero");
    PASS();
}

//The batch-3 shape: a waiter that has arrived (not last) parks in
//WaitForCompletion holding NOTHING, until the elected collector
//completes. Both windows must be up BEFORE the request (request
//snapshots WindowThreadCount as the expected arrival count), so the
//worker gates on a main-side flag; CHECKs run after join.
static void test_waiter_blocks_until_complete()
{
    TEST(waiter_blocks_until_complete);
    GcWorldStop stop;
    std::atomic<bool> windowUp{false};
    std::atomic<bool> requestNow{false};
    std::atomic<bool> done{false};
    std::atomic<uint64_t> epochOut{0};
    std::thread worker([&] {
        GcWorldStop::ExecWindowGuard guard(stop);
        windowUp.store(true);
        while (!requestNow.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const uint64_t epoch = stop.RequestWorldStop();   //expected = 2
        const bool last = stop.Arrive(epoch);   //arrive FIRST, publish
        //after — publishing the epoch before arriving would let main
        //arrive first and turn the election into a race.
        epochOut.store(epoch);
        if (!last)
            stop.WaitForCompletion(epoch);   // released-GIL wait shape
        done.store(true);
    });
    GcWorldStop::ExecWindowGuard ownGuard(stop);
    while (!windowUp.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool bothCounted = stop.WindowThreadCount() == 2;
    requestNow.store(true);   //both windows up — the request sees two
    while (epochOut.load() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool mainElected = stop.Arrive(epochOut.load());
    const bool parkedPreComplete = !done.load();
    stop.Complete(epochOut.load());
    worker.join();
    CHECK(bothCounted, "both windows counted");
    CHECK(mainElected, "main (2nd window) is the last arriver");
    CHECK(parkedPreComplete, "waiter still parked pre-Complete");
    CHECK(done.load(), "waiter released by Complete");
    PASS();
}

int main()
{
    test_single_requester_is_last();
    test_three_arrivers_elect_exactly_one();
    test_supersede_folds_old_epoch();
    test_window_depth_and_suspend();
    test_waiter_blocks_until_complete();
    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
