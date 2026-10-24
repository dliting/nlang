/*---
    test_domainlocks.cpp - K8 Task 1: domain lock set tests.

    Real threads drive the lock set directly: mutual exclusion, per-domain
    reentrant nesting markers, the fixed acquisition-order tripwire,
    cross-domain overlap, and per-domain telemetry counters.
---*/
#include "VmDomainLocks.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

static void MutualExclusion() {
    TEST(mutual_exclusion);
    DomainLockSet set;
    std::atomic<int> inCritical{0};
    std::atomic<int> violations{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                DomainGuard g(set, DomainId::Container);
                if (inCritical.fetch_add(1) != 0) ++violations;
                inCritical.fetch_sub(1);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    for (auto& w : workers) w.join();
    CHECK(violations.load() == 0, "critical section overlapped");
    PASS();
}

static void ReentrantNesting() {
    TEST(reentrant_nesting);
    DomainLockSet set;
    {
        DomainGuard outer(set, DomainId::Container);
        {
            DomainGuard inner(set, DomainId::Container);   //marker no-op
        }
        //inner destructor must not release: another thread still cannot lock
        std::thread probe([&]() {
            //try_lock from another thread while outer holds
            std::atomic<bool> locked{false};
            std::thread([&]() {
                if (set.Mutex(DomainId::Container).try_lock()) {
                    set.Mutex(DomainId::Container).unlock();
                    locked = true;
                }
            }).join();
            CHECK(!locked.load(), "inner guard released the outer lock");
        });
        probe.join();
    }
    std::mutex& m = set.Mutex(DomainId::Container);
    CHECK(m.try_lock(), "outer guard did not release");
    m.unlock();
    PASS();
}

static void LockOrderTripwire() {
    TEST(lock_order_tripwire);
    DomainLockSet set;
    std::atomic<int> tripped{0};
    set.SetTripwireCallback([&](DomainId attempted) {
        tripped.fetch_add(1);
        return attempted;   //test mode: record and continue
    });
    {
        DomainGuard ok(set, DomainId::Sync);        //holds Sync (order 1)
        DomainGuard reverse(set, DomainId::Container);   //lower under higher = violation
        CHECK(tripped.load() == 1, "reverse acquisition did not trip");
    }
    {
        DomainGuard ok(set, DomainId::Container);   //holds Container (order 0)
        DomainGuard forward(set, DomainId::Sync);   //higher under lower = legal
        CHECK(tripped.load() == 1, "legal forward order tripped the wire");
    }
    set.SetTripwireCallback(nullptr);   //restore production terminate
    PASS();
}

static void DistinctDomainsOverlap() {
    TEST(distinct_domains_overlap);
    DomainLockSet set;
    std::atomic<bool> aHeld{false}, bHeld{false}, overlapSeen{false};
    std::thread a([&]() {
        DomainGuard g(set, DomainId::Container);
        aHeld = true;
        for (int i = 0; i < 200 && !overlapSeen.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    std::thread b([&]() {
        while (!aHeld.load()) std::this_thread::yield();
        DomainGuard g(set, DomainId::Sync);
        if (aHeld.load()) overlapSeen = true;
        bHeld = true;
    });
    a.join();
    b.join();
    CHECK(overlapSeen.load(), "container and sync locks never overlapped");
    PASS();
}

static void TelemetryCounts() {
    TEST(telemetry_counts);
    DomainLockSet set;
    {
        DomainGuard g(set, DomainId::Stream);
    }
    CHECK(set.Acquisitions(DomainId::Stream) >= 1, "acquisition not counted");
    CHECK(set.Contended(DomainId::Stream) == 0, "uncontended acquire marked contended");

    const int kRounds = 2000;
    std::atomic<bool> go{false};
    std::vector<std::thread> racers;
    for (int t = 0; t < 2; ++t) {
        racers.emplace_back([&]() {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < kRounds; ++i) {
                DomainGuard g(set, DomainId::Growth);
            }
        });
    }
    go = true;
    for (auto& r : racers) r.join();
    CHECK(set.Acquisitions(DomainId::Growth) >= 2 * kRounds, "racing acquisitions not counted");
    //Contention accounting: one holder provably owns the mutex for a
    //fixed window while a second thread bursts acquire attempts through
    //it. Empty-critical-section racers above can pass each other without
    //ever overlapping (observed flake); a 1ms hold with 10us-attempt
    //bursts makes a missed collision require total scheduler starvation.
    const int kHoldRounds = 50;
    std::atomic<bool> stop{false};
    std::thread holder([&]() {
        for (int r = 0; r < kHoldRounds; ++r) {
            DomainGuard g(set, DomainId::Growth);
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        stop.store(true);
    });
    std::thread burster([&]() {
        while (!stop.load()) {
            DomainGuard g(set, DomainId::Growth);
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });
    holder.join();
    burster.join();
    CHECK(set.Contended(DomainId::Growth) > 0, "contended acquisitions never recorded");
    CHECK(set.WaitUs(DomainId::Growth) > 0, "wait time never recorded");
    PASS();
}

int main() {
    std::cerr << "test_domainlocks:\n";
    MutualExclusion();
    ReentrantNesting();
    LockOrderTripwire();
    DistinctDomainsOverlap();
    TelemetryCounts();
    std::cerr << "  " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
