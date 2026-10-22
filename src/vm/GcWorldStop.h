/*---
    GcWorldStop.h — GC stop-the-world rendezvous (concurrency K6, 2026-10-22)

    Last-arriver-collects handshake: a requester advances the request
    epoch; every live bytecode-evaluation window arrives; the LAST
    arriver runs the collection and completes the epoch; waiters are
    released by the comparison "completedEpoch >= epoch" (epochs are a
    monotonic uint64 and never reset to zero mid-flight, so a superseded
    epoch's waiters are released when the superseding epoch completes).

    Lock contract: the internal mutex is a LEAF lock — never acquire the
    GIL (or any other lock) while holding it. Acquiring it while holding
    the GIL is fine (GIL -> gcStop is the one legal order).
    WaitForCompletion must NOT be called while holding the GIL: waiters
    sleep on the condition variable, and an arrived party that needs the
    GIL to reach its safepoint would deadlock.

    K6 degeneracy: while the GIL exists, at most one thread executes
    bytecode, so the product-path rendezvous always elects the requester
    itself (inline collection). The multi-party shape is exercised by
    this class's white-box tests with real threads; batch 3 (frame-stack
    privatization) turns the protocol into the real park-and-wait path.
    All window-count transitions happen under the GIL in product paths
    (guards enter/exit inside GIL-held windows; park suspends before the
    GIL release, resume happens after the reacquire), so the
    Request->Arrive pair cannot observe a changing denominator.
---*/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace nlang {

class GcWorldStop {
public:
    //One instance per GIL-held bytecode-evaluation window per thread
    //(carrier windows, host CallByName nested windows, re-entrant
    //callback windows — nesting raises the thread-local depth but
    //counts the thread once). Suspend/resume bracket blocking-native
    //park/restore inside one window.
    class ExecWindowGuard {
    public:
        explicit ExecWindowGuard(GcWorldStop& stop) : m_rStop(stop) {
            if (++t_evalDepth == 1)
                m_rStop.m_windowThreads.fetch_add(1,
                    std::memory_order_acq_rel);
        }
        ~ExecWindowGuard() {
            if (--t_evalDepth == 0)
                m_rStop.m_windowThreads.fetch_sub(1,
                    std::memory_order_acq_rel);
        }
        ExecWindowGuard(const ExecWindowGuard&) = delete;
        ExecWindowGuard& operator=(const ExecWindowGuard&) = delete;
    private:
        GcWorldStop& m_rStop;
    };

    //Advance to a fresh epoch and snapshot the expected arriver count
    //(the live window count). Supersedes any incomplete epoch: its
    //stale arrivals are refused by the epoch check in Arrive.
    uint64_t RequestWorldStop() {
        std::lock_guard<std::mutex> g(m_mutex);
        ++m_requestEpoch;
        m_expected = static_cast<size_t>(
            m_windowThreads.load(std::memory_order_acquire));
        m_arrived = 0;
        return m_requestEpoch;
    }

    //Register this window's arrival. Returns true for the LAST arriver
    //(strict equality: an expected==0 wiring bug — the requester's own
    //window missing from the count — yields false, and the product path
    //turns that into its accounting tripwire instead of a silent pass).
    //A stale (superseded) epoch reports false: not last, wait instead.
    bool Arrive(uint64_t epoch) {
        std::lock_guard<std::mutex> g(m_mutex);
        if (epoch != m_requestEpoch)
            return false;
        ++m_arrived;
        return m_arrived == m_expected;
    }

    //Park until the epoch completes. Precondition: caller does NOT hold
    //the GIL (file-header contract). The >= comparison releases waiters
    //of superseded epochs when the superseding epoch completes.
    void WaitForCompletion(uint64_t epoch) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait(lk,
            [this, epoch] { return m_completedEpoch >= epoch; });
    }

    //Publish completion (the collector, after CollectGarbage).
    void Complete(uint64_t epoch) {
        {
            std::lock_guard<std::mutex> g(m_mutex);
            if (epoch > m_completedEpoch)
                m_completedEpoch = epoch;
        }
        m_cv.notify_all();
    }

    //Between runs (no in-flight rendezvous, no waiters). Epoch numbering
    //restarts; m_windowThreads is NOT touched — live guards own their
    //transitions, and none exist between runs.
    void Reset() {
        std::lock_guard<std::mutex> g(m_mutex);
        m_requestEpoch = 0;
        m_completedEpoch = 0;
        m_expected = 0;
        m_arrived = 0;
    }

    //The blocking-native park/restore pair, inside one window: the
    //thread stops executing bytecode (GIL away, frames parked), so it
    //leaves the window count; a re-entrant callback re-enters RunFrames
    //and re-counts itself through a nested guard.
    void SuspendEvalWindow() {
        if (--t_evalDepth == 0)
            m_windowThreads.fetch_sub(1, std::memory_order_acq_rel);
    }
    void ResumeEvalWindow() {
        if (++t_evalDepth == 1)
            m_windowThreads.fetch_add(1, std::memory_order_acq_rel);
    }

    int WindowThreadCount() const {
        return m_windowThreads.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex m_mutex;         //leaf lock (file-header contract)
    std::condition_variable m_cv;
    uint64_t m_requestEpoch = 0;        //current in-flight epoch (single)
    uint64_t m_completedEpoch = 0;      //monotonic; >= comparison semantics
    size_t m_expected = 0;              //arriver count snapshot at request
    size_t m_arrived = 0;               //arrivals this epoch
    std::atomic<int> m_windowThreads{0};//threads with depth > 0
    static thread_local int t_evalDepth;
};

} // namespace nlang
