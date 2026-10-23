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

    K7 (2026-10-23): the thread registry replaces the K6 window count.
    Threads register at their outermost evaluation boundary and publish
    their slot's evaluation stack (the scan surface); an executing bit
    per thread is the rendezvous denominator; Rule A (start executing)
    waits out an in-flight stop, Rule B (stop executing) arrives under
    it. The registry protocol and the K6 window protocol share the
    epoch members but no caller mixes them on one instance (the K6
    family is removed once the switch-over completes).
---*/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace nlang {

struct CallFrame;   //published scan-surface root (CallFrame.h)

//Registration handle. Generation-stamped: a stale token — a zombie
//carrier from a torn-down run, or a Reset-retired entry whose index a
//later registration reused — identifies nothing and is ignored.
struct GcThreadToken {
    uint64_t generation = 0;   //0 = never issued
    size_t index = 0;
};

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

    //The blocking-native park/restore pair. Park suspends the thread's
    //entire evaluation stack — outer host-window guards included (a
    //CallByName window nests its own guard around RunFrames') — so it
    //saves the current depth and leaves the window count; the thread
    //runs native code only (a re-entrant callback re-counts itself from
    //zero through its own guards). Restore reinstates the saved depth:
    //saves and restores are strictly nested (LIFO), and at restore the
    //depth is always zero (callback guards unwound with their windows),
    //so the stack top is the matching save.
    void SuspendEvalWindow() {
        if (t_evalDepth > 0) {
            t_suspendedDepths.push_back(t_evalDepth);
            t_evalDepth = 0;
            m_windowThreads.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
    void ResumeEvalWindow() {
        if (!t_suspendedDepths.empty()) {
            t_evalDepth = t_suspendedDepths.back();
            t_suspendedDepths.pop_back();
            m_windowThreads.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    int WindowThreadCount() const {
        return m_windowThreads.load(std::memory_order_acquire);
    }

    //--- K7: thread registry + executing-bit rendezvous ---

    //Register the calling thread at its OUTERMOST evaluation boundary
    //(carrier-loop entry, host-bridge outermost entry), publishing its
    //slot's evaluation stack — the vector lives in the thread's
    //carrier/fallback slot and is stable for the thread's lifetime.
    //The collector scans every live entry (RegisteredStacks). Double
    //registration on one instance is an outermost-rule violation and
    //trips the fail-loud terminate.
    GcThreadToken RegisterThread(std::vector<CallFrame>* evalStack);

    //Retire a registered entry (the owner's outermost boundary exit).
    //A stale token (Reset-retired, index reused) is a no-op; retiring
    //an entry that still has its executing bit set is a protocol bug
    //and trips the terminate (carriers unregister at loop exit,
    //parked).
    void UnregisterThread(GcThreadToken token);

    bool IsRegistered() const;  //this thread's own membership
    bool ExecutingSelf() const; //this thread's own executing bit

    //Rule A — call where the thread STARTS executing bytecode (dispatch
    //entry after queue selection, bridge re-entry, blocking-native
    //return). Waits out any in-flight stop FIRST on the rendezvous cv
    //(caller must hold no global lock — file-header contract), then
    //flips the bit on: flipping on during a stop would race the
    //collector's scan of this thread's published stack. Membership is
    //a precondition; unregistered use terminates (fail-loud).
    void BeginExecuting();

    //Rule B — call where the thread STOPS executing bytecode (park,
    //blocking-native entry, dispatch end). Flips the bit off; if a stop
    //is in flight, this transition IS the thread's arrival — flip and
    //arrival share one critical section, so a concurrent snapshot can
    //never miss either half. Returns true when this arrival completed
    //the expected set: the CALLER is the last arriver and must collect
    //(under the global lock) and call Complete().
    bool EndExecuting();

    //Fast safepoint poll (lock-free atomic read).
    bool StopInFlight() const {
        return m_stopActive.load(std::memory_order_acquire);
    }

    //GC trigger at a safepoint — race-closed internally. Under the
    //leaf mutex, a stop already in flight is JOINED as an arrival
    //(Rule B shape): dual initiation never advances a second epoch and
    //never trips a crash. Only a thread finding no stop in flight
    //advances the epoch, snapshots the executing count (self included),
    //flips self off, and arrives — snapshot and self-arrival in one
    //critical section. Returns true = last arriver → collect inline
    //and call Complete(). False → call WaitForCompletion().
    bool InitiateAndArrive();

    //Wait until the request epoch observed at entry completes (>=
    //comparison: any later completion releases). The epoch is captured
    //under the leaf mutex; caller must hold no global lock.
    void WaitForCompletion();
    //Deactivate the stop and publish completion (the collector after
    //CollectGarbage; also the cancellation entry — a completer that
    //never arrived releases everyone parked on the epoch).
    void Complete();

    //Between runs: zeroes the epoch accounting and retires ALL entries
    //unconditionally (grace-period zombie carriers may still be
    //registered — an empty assert would fire on them). Retired entries
    //are marked dead IN PLACE: the deque never shrinks, so a detached
    //zombie's self-entry pointer stays valid (and inert).
    void Reset();

    //Collector-side snapshot of the scan surface — the published
    //stacks of live registered threads, in registration order.
    std::vector<std::vector<CallFrame>*> RegisteredStacks() const;

private:
    //Registry entry. `executing` and `live` are atomics because the
    //owning thread reads them without the leaf lock (ExecutingSelf /
    //IsRegistered); all protocol mutations happen under the lock.
    struct ThreadEntry {
        std::vector<CallFrame>* evalStack = nullptr;  //published root
        std::atomic<bool> executing{false};
        std::atomic<bool> live{false};
        uint64_t generation = 0;      //token staleness stamp
    };

    //This thread's own entry on THIS instance (null for unregistered
    //threads and for threads registered with another instance — the
    //pair is rewritten at each RegisterThread, matching the K6
    //thread-local precedent of one executor per thread at a time).
    ThreadEntry* SelfEntry() {
        if (t_selfStop != this)
            return nullptr;
        return t_selfEntry;
    }

    //Fail-loud membership/precondition tripwire (Release-active, the
    //INV-2 style): a silent return would let an unregistered thread
    //execute bytecode outside the rendezvous denominator.
    [[noreturn]] static void Tripwire(const char* what);

    mutable std::mutex m_mutex;         //leaf lock (file-header contract)
    std::condition_variable m_cv;
    uint64_t m_requestEpoch = 0;        //current in-flight epoch (single)
    uint64_t m_completedEpoch = 0;      //monotonic; >= comparison semantics
    size_t m_expected = 0;              //arriver count snapshot at request
    size_t m_arrived = 0;               //arrivals this epoch
    std::atomic<int> m_windowThreads{0};//threads with depth > 0
    std::deque<ThreadEntry> m_threads;  //address-stable: never shrinks
    std::deque<size_t> m_freeIndices;   //retired slots, LIFO reuse
    uint64_t m_nextGeneration = 1;      //monotonic; 0 = never issued
    size_t m_executingCount = 0;        //registry threads with bit set
    std::atomic<bool> m_stopActive{false};  //stop in flight (fast poll)
    static thread_local int t_evalDepth;
    static thread_local std::vector<int> t_suspendedDepths; //park save stack
    static thread_local GcWorldStop* t_selfStop;
    static thread_local ThreadEntry* t_selfEntry;
};

} // namespace nlang
