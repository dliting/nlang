/*---
    test_gcworldstop.cpp - K7 Task 2: GcWorldStop tests.

    Real threads drive the multi-party rendezvous directly (no GIL, no
    executor): the K7 thread registry — registration tokens, the
    executing bit, Rule A/B fusion, dual-initiation closure, zombie
    tolerance, and the scan surface. (The K6 evaluation-window
    accounting tests were deleted with the mechanism in K7 Task 4.)
---*/
#include "CallFrame.h"
#include "GcWorldStop.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
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

// ---- K7 Task 2: thread registry + executing-bit rendezvous ----

//Bounded poll: returns false on timeout so a protocol bug fails a
//CHECK instead of hanging the suite (the K6 tests above poll
//unbounded; the new API's tests stay fail-fast).
static bool waitFor(const std::function<bool()>& pred, int timeoutMs)
{
    for (int i = 0; i < timeoutMs; ++i) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

//1. Single-thread degenerate shape: the only executing thread that
//initiates is necessarily the last arriver — inline collect (true).
static void test_k7_single_thread_inline_collect()
{
    TEST(k7_single_thread_inline_collect);
    GcWorldStop stop;
    std::vector<CallFrame> stack;
    const GcThreadToken token = stop.RegisterThread(&stack);
    CHECK(stop.IsRegistered(), "registered after RegisterThread");
    stop.BeginExecuting();
    CHECK(stop.ExecutingSelf(), "executing after BeginExecuting");
    CHECK(stop.InitiateAndArrive(), "sole executer is the last arriver");
    stop.Complete();
    stop.WaitForCompletion();   //already complete — returns at once
    CHECK(!stop.StopInFlight(), "stop inactive after Complete");
    CHECK(!stop.ExecutingSelf(), "initiation flipped self off");
    //Plain flip-off shape (no initiation involved):
    stop.BeginExecuting();
    CHECK(stop.ExecutingSelf(), "re-entry flips on");
    CHECK(!stop.EndExecuting(), "plain flip-off with no stop in flight");
    CHECK(!stop.ExecutingSelf(), "not executing after EndExecuting");
    stop.UnregisterThread(token);
    CHECK(!stop.IsRegistered(), "unregistered");
    PASS();
}

//1b. Telemetry: CompletedStops counts one per released stop epoch
//(the inline-collect cycle completes one); Reset zeroes it with the
//epoch accounting.
static void test_k7_completed_stops_telemetry()
{
    TEST(k7_completed_stops_telemetry);
    GcWorldStop stop;
    std::vector<CallFrame> stack;
    const GcThreadToken token = stop.RegisterThread(&stack);
    stop.BeginExecuting();
    CHECK(stop.CompletedStops() == 0, "no stops before initiation");
    CHECK(stop.InitiateAndArrive(), "sole executer is the last arriver");
    stop.Complete();
    stop.WaitForCompletion();
    CHECK(stop.CompletedStops() == 1,
          "one completed stop after the inline-collect cycle");
    stop.BeginExecuting();
    CHECK(stop.InitiateAndArrive(), "second epoch initiates cleanly");
    stop.Complete();
    stop.WaitForCompletion();
    CHECK(stop.CompletedStops() == 2, "second epoch counted");
    stop.Reset();
    CHECK(stop.CompletedStops() == 0, "Reset zeroes the stop counter");
    (void)token;
    PASS();
}

//2. Real two-thread rendezvous: the initiator (T1=main, not last)
//parks in WaitForCompletion; T2's Rule-B EndExecuting completes the
//set (true), collects, and Completes. Both re-enter cleanly after.
static void test_k7_two_thread_rendezvous()
{
    TEST(k7_two_thread_rendezvous);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    std::atomic<bool> t2Executing{false}, initiated{false};
    std::atomic<bool> t2WasLast{false};
    std::thread t2([&] {
        const GcThreadToken token2 = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        //Wait for main's request to be in flight BEFORE arriving (the
        //flag is set after InitiateAndArrive returned, so the stop is
        //active under the lock by then).
        waitFor([&] { return initiated.load(); }, 5000);
        t2WasLast.store(stop.EndExecuting());
        if (t2WasLast.load())
            stop.Complete();   //last arriver collects then completes
        stop.UnregisterThread(token2);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "T2 executing before initiation");
    const bool initiatorNotLast = !stop.InitiateAndArrive();
    initiated.store(true);
    //T2 arrives and completes; main parks — if T2 raced ahead, main's
    //wait predicate already holds. Both ends are correct.
    stop.WaitForCompletion();
    t2.join();
    CHECK(initiatorNotLast, "initiator not last (two executing)");
    CHECK(t2WasLast.load(), "T2's Rule-B arrival completed the set");
    //Re-entry after the rendezvous is clean:
    stop.BeginExecuting();
    CHECK(stop.ExecutingSelf(), "re-entry after rendezvous");
    CHECK(!stop.EndExecuting(), "plain flip-off after re-entry");
    stop.UnregisterThread(tokenMain);
    PASS();
}

//3. Three executing threads: the initiator self-arrives (1 of 3), two
//Rule-B EndExecuting transitions arrive 2nd and 3rd — exactly one of
//the two is elected last (arrival order decides, under the leaf lock).
static void test_k7_rule_b_last_arriver_election()
{
    TEST(k7_rule_b_last_arriver_election);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2, stackT3;
    std::atomic<bool> t2Executing{false}, t3Executing{false};
    std::atomic<bool> initiated{false};
    std::atomic<int> lastCount{0};
    auto arriver = [&](std::vector<CallFrame>* stack,
                       std::atomic<bool>& executing) {
        const GcThreadToken token = stop.RegisterThread(stack);
        stop.BeginExecuting();
        executing.store(true);
        waitFor([&] { return initiated.load(); }, 5000);
        if (stop.EndExecuting()) {
            lastCount.fetch_add(1);
            stop.Complete();   //elected collector collects then completes
        }
        stop.UnregisterThread(token);
    };
    std::thread t2(arriver, &stackT2, std::ref(t2Executing));
    std::thread t3(arriver, &stackT3, std::ref(t3Executing));
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load() && t3Executing.load(); },
                  5000), "both arrivers executing before initiation");
    const bool initiatorNotLast = !stop.InitiateAndArrive();   //1 of 3
    initiated.store(true);
    stop.WaitForCompletion();
    t2.join();
    t3.join();
    CHECK(initiatorNotLast, "initiator not last (three executing)");
    CHECK(lastCount.load() == 1, "exactly one Rule-B arrival elected");
    stop.UnregisterThread(tokenMain);
    PASS();
}

//4. Rule A deferral: a registered non-executing thread's
//BeginExecuting BLOCKS while a stop is in flight (the elected
//collector deliberately holds the window open) and proceeds only
//after Complete.
static void test_k7_rule_a_waits_out_inflight_stop()
{
    TEST(k7_rule_a_waits_out_inflight_stop);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2, stackT3;
    std::atomic<bool> t2Executing{false}, initiated{false};
    std::atomic<bool> holdRelease{false}, t2WasLast{false};
    std::atomic<bool> t3Executing{false}, t3Done{false};
    std::thread t2([&] {   //the elected collector — holds the window
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return initiated.load(); }, 5000);
        t2WasLast.store(stop.EndExecuting());
        if (t2WasLast.load()) {
            waitFor([&] { return holdRelease.load(); }, 5000);
            stop.Complete();   //window stays open until released
        }
        stop.UnregisterThread(token);
    });
    std::thread t3([&] {   //registered, NOT executing — Rule A waiter
        const GcThreadToken token = stop.RegisterThread(&stackT3);
        waitFor([&] { return initiated.load(); }, 5000);
        stop.BeginExecuting();   //must park: stop is in flight
        t3Executing.store(true);
        stop.EndExecuting();   //no stop by then — plain flip-off
        stop.UnregisterThread(token);
        t3Done.store(true);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "T2 executing before initiation");
    const bool initiatorNotLast = !stop.InitiateAndArrive();   //1 of 2
    initiated.store(true);
    //T2 is necessarily the last arriver here (main already arrived
    //under the request's lock) and is holding the window open — T3's
    //BeginExecuting cannot have completed yet. Main does NOT park
    //here (it must stay runnable to release the hold below); its own
    //release is not part of this test's assertions.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool t3StillParked = !t3Executing.load();
    holdRelease.store(true);
    CHECK(waitFor([&] { return t3Done.load(); }, 5000),
          "T3 proceeded after Complete");
    t2.join();
    t3.join();
    CHECK(initiatorNotLast, "initiator not last (two executing)");
    CHECK(t2WasLast.load(), "T2 elected collector");
    CHECK(t3StillParked, "T3's BeginExecuting parked while in flight");
    CHECK(t3Executing.load(), "T3 executing after the wait-out");
    stop.UnregisterThread(tokenMain);
    PASS();
}

//5. A registered-not-executing thread unregisters during an in-flight
//stop: not part of the denominator, so no arrival — the rendezvous
//still completes with the original two.
static void test_k7_unregister_not_in_denominator()
{
    TEST(k7_unregister_not_in_denominator);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2, stackT3;
    std::atomic<bool> t2Executing{false}, t3Registered{false};
    std::atomic<bool> t3Unregistered{false}, initiated{false};
    std::atomic<bool> snapshotDone{false}, t2WasLast{false};
    std::atomic<bool> mainReleased{false};
    std::thread t2([&] {
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return initiated.load(); }, 5000);
        t2WasLast.store(stop.EndExecuting());
        if (t2WasLast.load())
            stop.Complete();
        //Stay registered until main snapshots the scan surface (the
        //snapshot must see both members).
        waitFor([&] { return snapshotDone.load(); }, 5000);
        stop.UnregisterThread(token);
    });
    std::thread t3([&] {   //registered only — never executes
        const GcThreadToken token = stop.RegisterThread(&stackT3);
        t3Registered.store(true);
        waitFor([&] { return initiated.load(); }, 5000);
        stop.UnregisterThread(token);   //mid-flight, not executing
        t3Unregistered.store(true);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] {
        return t2Executing.load() && t3Registered.load();
    }, 5000), "both T2 executing and T3 registered before initiation");
    const bool initiatorNotLast = !stop.InitiateAndArrive();   //1 of 2
    initiated.store(true);
    //Wait for T3's mid-flight unregister, then snapshot the surface.
    CHECK(waitFor([&] { return t3Unregistered.load(); }, 5000),
          "T3 unregistered during the stop");
    const size_t stacksAfter = stop.RegisteredStacks().size();
    snapshotDone.store(true);
    stop.WaitForCompletion();
    mainReleased.store(true);
    t2.join();
    t3.join();
    CHECK(initiatorNotLast, "initiator not last (two executing)");
    CHECK(stacksAfter == 2, "scan surface keeps the two members");
    CHECK(t2WasLast.load(), "rendezvous completed by T2 alone");
    CHECK(mainReleased.load(), "initiator released");
    stop.UnregisterThread(tokenMain);
    PASS();
}

//6. Complete-by-non-arriver (the cancellation shape): a thread parked
//on the in-flight epoch is released by a Complete from a thread that
//never arrived; a LATE Rule-B EndExecuting afterwards is a clean
//no-op (returns false, no arrival, no counter corruption).
static void test_k7_complete_releases_waiter_late_end_noop()
{
    TEST(k7_complete_releases_waiter_late_end_noop);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    std::atomic<bool> t2Executing{false}, inFlight{false};
    std::atomic<bool> mainParked{false}, mainReleased{false};
    std::atomic<bool> stillParkedAtCancel{false}, lateEndResult{true};
    std::thread t2([&] {
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return inFlight.load() && mainParked.load(); }, 5000);
        //Main is necessarily inside the cv wait (only this thread can
        //complete the epoch) — observe, then release.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stillParkedAtCancel.store(!mainReleased.load());
        stop.Complete();   //canceller never arrived
        waitFor([&] { return mainReleased.load(); }, 5000);
        lateEndResult.store(stop.EndExecuting());   //late — no stop
        stop.UnregisterThread(token);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "T2 executing before initiation");
    const bool initiatorNotLast = !stop.InitiateAndArrive();   //1 of 2
    inFlight.store(true);
    mainParked.store(true);
    stop.WaitForCompletion();   //parks: only T2 can complete
    mainReleased.store(true);
    t2.join();
    CHECK(initiatorNotLast, "initiator not last (two executing)");
    CHECK(stillParkedAtCancel.load(), "waiter parked until Complete");
    CHECK(mainReleased.load(), "waiter released by Complete");
    CHECK(!lateEndResult.load(), "late Rule-B after completion is a no-op");
    stop.UnregisterThread(tokenMain);   //initiator already flipped off
    PASS();
}

//7. Counter integrity across the late no-op: after a cancelled stop
//and a late Rule-B no-op, a FRESH full rendezvous still elects exactly
//the true last arriver — expected/arrived were not corrupted.
static void test_k7_fresh_election_after_noop()
{
    TEST(k7_fresh_election_after_noop);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    std::atomic<bool> t2Executing{false}, phase1Done{false};
    std::atomic<bool> t2Ready2{false}, goArrive{false};
    std::atomic<bool> lateEndResult{true}, t2WasLast{false};
    std::thread t2([&] {
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return phase1Done.load(); }, 5000);
        lateEndResult.store(stop.EndExecuting());   //late — no stop
        stop.BeginExecuting();   //re-enter for the fresh cycle
        t2Ready2.store(true);
        waitFor([&] { return goArrive.load(); }, 5000);
        t2WasLast.store(stop.EndExecuting());
        if (t2WasLast.load())
            stop.Complete();
        stop.UnregisterThread(token);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "T2 executing before phase 1");
    //Phase 1: initiate, then cancel from the initiator itself.
    const bool p1NotLast = !stop.InitiateAndArrive();   //1 of 2
    stop.Complete();   //cancel — T2 never arrived
    phase1Done.store(true);
    //Phase 2: T2 re-executes, main re-enters (it flipped off at the
    //phase-1 initiation) — a fresh full rendezvous.
    CHECK(waitFor([&] { return t2Ready2.load(); }, 5000),
          "T2 re-entered for phase 2");
    stop.BeginExecuting();
    const bool p2NotLast = !stop.InitiateAndArrive();   //1 of 2
    goArrive.store(true);
    stop.WaitForCompletion();
    t2.join();
    CHECK(p1NotLast, "phase-1 initiator not last");
    CHECK(p2NotLast, "phase-2 initiator not last");
    CHECK(!lateEndResult.load(), "late Rule-B after cancel is a no-op");
    CHECK(t2WasLast.load(), "fresh cycle elected the true last arriver");
    stop.UnregisterThread(tokenMain);   //main off since its phase-2 initiation
    PASS();
}

//8. Wait contracts: (a) the degenerate inline rendezvous is legal
//UNDER a held global lock (single-carrier product shape); (b) a thread
//parked in WaitForCompletion holds NOTHING — another thread acquires
//the stand-in global lock freely while it waits.
static void test_k7_wait_contracts()
{
    TEST(k7_wait_contracts);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    std::timed_mutex standInGil;   //stands in for the executor GIL
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    //8a: inline collect under the held stand-in lock.
    bool inlineLast = false;
    {
        std::lock_guard<std::timed_mutex> g(standInGil);
        inlineLast = stop.InitiateAndArrive();   //expected=1 -> true
        stop.Complete();
    }
    stop.BeginExecuting();   //re-enter after 8a's self-flip-off
    //8b: parked waiter holds nothing.
    std::atomic<bool> t2Executing{false};
    std::atomic<bool> mainParked{false}, mainReleased{false};
    std::atomic<bool> t2GotLock{false}, t2WasLast{false};
    std::thread t2([&] {
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return mainParked.load(); }, 5000);
        t2GotLock.store(standInGil.try_lock_for(
            std::chrono::milliseconds(200)));
        if (t2GotLock.load())
            standInGil.unlock();
        t2WasLast.store(stop.EndExecuting());
        if (t2WasLast.load())
            stop.Complete();
        stop.UnregisterThread(token);
    });
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "T2 executing before initiation");
    const bool notLast = !stop.InitiateAndArrive();   //1 of 2
    mainParked.store(true);
    stop.WaitForCompletion();
    mainReleased.store(true);
    t2.join();
    CHECK(inlineLast, "degenerate rendezvous completes inline under lock");
    CHECK(notLast, "two-thread request is not self-completing");
    CHECK(t2GotLock.load(), "parked waiter held no global lock");
    CHECK(mainReleased.load(), "waiter released by the arriver's Complete");
    stop.UnregisterThread(tokenMain);   //main off since 8b's initiation
    PASS();
}

//9. Scan surface: RegisteredStacks exposes exactly the live entries'
//published pointers; IsRegistered brackets membership.
static void test_k7_scan_surface()
{
    TEST(k7_scan_surface);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    CHECK(!stop.IsRegistered(), "not registered initially");
    CHECK(stop.RegisteredStacks().empty(), "scan surface empty");
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    CHECK(stop.IsRegistered(), "membership after register");
    std::atomic<bool> t2Registered{false}, t2Unregistered{false};
    GcThreadToken tokenT2{};
    std::thread t2([&] {
        tokenT2 = stop.RegisterThread(&stackT2);
        t2Registered.store(true);
        waitFor([&] { return t2Unregistered.load(); }, 5000);
        stop.UnregisterThread(tokenT2);
    });
    CHECK(waitFor([&] { return t2Registered.load(); }, 5000),
          "T2 registered");
    {
        const auto stacks = stop.RegisteredStacks();
        CHECK(stacks.size() == 2, "two live entries");
        CHECK(stacks[0] == &stackMain && stacks[1] == &stackT2,
              "published pointers in registration order");
    }
    t2Unregistered.store(true);
    CHECK(waitFor([&] {
        return stop.RegisteredStacks().size() == 1;
    }, 5000), "T2's entry retired");
    CHECK(stop.RegisteredStacks()[0] == &stackMain,
          "surviving entry is main's");
    t2.join();
    stop.UnregisterThread(tokenMain);
    CHECK(stop.RegisteredStacks().empty(), "surface empty after teardown");
    PASS();
}

//10. Dual initiation: two executing threads call InitiateAndArrive —
//whichever takes the leaf lock first initiates, the other JOINS as an
//arrival (never a second epoch). Exactly one of the two is elected;
//both come back.
static void test_k7_dual_initiation_joins()
{
    TEST(k7_dual_initiation_joins);
    GcWorldStop stop;
    std::vector<CallFrame> stackMain, stackT2;
    std::atomic<bool> t2Executing{false}, go{false}, t2Done{false};
    std::atomic<bool> t2WasLast{false};
    std::thread t2([&] {
        const GcThreadToken token = stop.RegisterThread(&stackT2);
        stop.BeginExecuting();
        t2Executing.store(true);
        waitFor([&] { return go.load(); }, 5000);
        const bool last = stop.InitiateAndArrive();
        t2WasLast.store(last);
        if (last)
            stop.Complete();
        else
            stop.WaitForCompletion();
        stop.UnregisterThread(token);
        t2Done.store(true);
    });
    const GcThreadToken tokenMain = stop.RegisterThread(&stackMain);
    stop.BeginExecuting();
    CHECK(waitFor([&] { return t2Executing.load(); }, 5000),
          "both executing before the race");
    go.store(true);
    const bool mainWasLast = stop.InitiateAndArrive();
    if (mainWasLast)
        stop.Complete();
    else
        stop.WaitForCompletion();
    CHECK(waitFor([&] { return t2Done.load(); }, 5000), "T2 came back");
    t2.join();
    CHECK(mainWasLast != t2WasLast.load(),
          "exactly one of the dual initiators elected");
    CHECK(!stop.StopInFlight(), "no stop left in flight");
    stop.UnregisterThread(tokenMain);   //main off since its initiation
    PASS();
}

//11. Token semantics: Reset retires entries unconditionally (no empty
//assert — zombies may still be registered); a stale token's
//UnregisterThread is a no-op even when its index was REUSED by a
//later registration (the generation stamp differs).
static void test_k7_reset_and_stale_tokens()
{
    TEST(k7_reset_and_stale_tokens);
    GcWorldStop stop;
    std::vector<CallFrame> stack1, stack2;
    const GcThreadToken token1 = stop.RegisterThread(&stack1);
    CHECK(stop.IsRegistered(), "registered pre-Reset");
    stop.Reset();   //entries still registered — must not terminate
    CHECK(!stop.IsRegistered(), "Reset retired the self entry");
    stop.UnregisterThread(token1);   //stale — no-op
    CHECK(stop.RegisteredStacks().empty(), "surface empty after Reset");
    const GcThreadToken token2 = stop.RegisterThread(&stack2);
    CHECK(stop.IsRegistered(), "fresh registration after Reset");
    CHECK(stop.RegisteredStacks().size() == 1, "one live entry");
    stop.UnregisterThread(token1);   //stale index reuse — must not
                                     //retire token2's entry
    CHECK(stop.IsRegistered(), "stale token did not retire the new one");
    CHECK(stop.RegisteredStacks().size() == 1, "entry still live");
    CHECK(stop.RegisteredStacks()[0] == &stack2, "the right entry");
    stop.UnregisterThread(token2);
    CHECK(stop.RegisteredStacks().empty(), "clean teardown");
    PASS();
}

#ifdef _WIN32
//Membership is a precondition, violated -> fail-loud terminate. The
//terminate is itself verified in a CHILD process (a direct call would
//kill the whole suite). CreateProcess, not system(): cmd.exe's broken
//AutoRun on this machine pollutes exit codes.
static bool runSelfExpectAbnormalExit(int crashCase)
{
    char exePath[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH))
        return false;
    char cmdLine[MAX_PATH + 32];
    std::snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --gcws-crash=%d",
                  exePath, crashCase);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW,   //own console: the tripwire
                                           //stderr must not leak here
                        nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return code != 0 && code != STILL_ACTIVE;
}
#endif

//12. ExecutingSelf transitions (false -> true -> false, re-entry),
//and the membership tripwires: Begin/EndExecuting on an unregistered
//thread terminate (verified in child processes).
static void test_k7_executing_self_and_tripwires()
{
    TEST(k7_executing_self_and_tripwires);
    GcWorldStop stop;
    std::vector<CallFrame> stack;
    const GcThreadToken token = stop.RegisterThread(&stack);
    CHECK(!stop.ExecutingSelf(), "not executing initially");
    stop.BeginExecuting();
    CHECK(stop.ExecutingSelf(), "executing between Begin/End");
    stop.EndExecuting();
    CHECK(!stop.ExecutingSelf(), "off after EndExecuting");
    stop.BeginExecuting();
    CHECK(stop.ExecutingSelf(), "re-entry flips on again");
    stop.EndExecuting();
    stop.UnregisterThread(token);
#ifdef _WIN32
    CHECK(runSelfExpectAbnormalExit(1),
          "BeginExecuting unregistered terminates");
    CHECK(runSelfExpectAbnormalExit(2),
          "EndExecuting unregistered terminates");
#else
    std::cerr << "(tripwire child-process checks Windows-only; skipped)";
#endif
    PASS();
}

//14. K8 Task 5: AssertAllArrived — the collector's quiescence gate.
//Green path: the elected last arriver calls it with every member
//arrived (single-member degenerate shape) and it returns normally.
//The pending-member shape (a registered member that never arrived)
//is the child-process crash case 3.
static void test_k8_assert_all_arrived_last_arriver()
{
    TEST(k8_assert_all_arrived_last_arriver);
    GcWorldStop stop;
    std::vector<CallFrame> stack;
    stop.RegisterThread(&stack);
    stop.BeginExecuting();
    CHECK(stop.InitiateAndArrive(), "sole member is the last arriver");
    stop.AssertAllArrived();   //must not trip
    stop.Complete();
    stop.BeginExecuting();     //re-enter before unregistering (parked exit)
    stop.EndExecuting();
    CHECK(true, "AssertAllArrived passed at full quiescence");
#ifdef _WIN32
    CHECK(runSelfExpectAbnormalExit(3),
          "AssertAllArrived with a pending member terminates");
#endif
    PASS();
}

int main(int argc, char** argv)
{
    //Child process: run one membership-crash case. Reaching the return
    //means the tripwire failed to fire — exit 0 so the parent FAILS.
    if (argc > 1 && std::strncmp(argv[1], "--gcws-crash=", 13) == 0) {
        GcWorldStop stop;
        std::vector<CallFrame> stack;
        const GcThreadToken token = stop.RegisterThread(&stack);
        stop.UnregisterThread(token);   //now unregistered
        switch (std::atoi(argv[1] + 13)) {
        case 1: stop.BeginExecuting(); break;
        case 2: stop.EndExecuting(); break;
        case 3: {
            //A second member registered and executing but never
            //arrives: AssertAllArrived must trip. The preamble retired
            //this thread's entry — re-register so the denominator is
            //two (poll count stays small: the Windows timer quantum
            //makes each 1ms sleep ~15ms).
            stop.RegisterThread(&stack);
            stop.BeginExecuting();
            std::atomic<bool> secondRegistered{false};
            std::thread pending([&stop, &secondRegistered]() {
                std::vector<CallFrame> second;
                stop.RegisterThread(&second);
                stop.BeginExecuting();
                secondRegistered.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::seconds(5));
            });
            waitFor([&stop, &secondRegistered]() {
                return secondRegistered.load(std::memory_order_acquire)
                    && stop.RegisteredStacks().size() == 2;
            }, 200);
            if (stop.InitiateAndArrive()) {
                pending.join();
                return 0;   //unexpected election — fail
            }
            stop.AssertAllArrived();   //must terminate
            pending.join();
            return 0;   //reached = tripwire failed to fire — fail
        }
        }
        return 0;
    }

    test_k7_single_thread_inline_collect();
    test_k7_completed_stops_telemetry();
    test_k7_two_thread_rendezvous();
    test_k7_rule_b_last_arriver_election();
    test_k7_rule_a_waits_out_inflight_stop();
    test_k7_unregister_not_in_denominator();
    test_k7_complete_releases_waiter_late_end_noop();
    test_k7_fresh_election_after_noop();
    test_k7_wait_contracts();
    test_k7_scan_surface();
    test_k7_dual_initiation_joins();
    test_k7_reset_and_stale_tokens();
    test_k7_executing_self_and_tripwires();
    test_k8_assert_all_arrived_last_arriver();
    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
