/*---
    GcWorldStop.h — GC stop-the-world rendezvous (concurrency K6→K7)

    Thread registry + executing-bit handshake: threads register at their
    outermost evaluation boundary and publish their slot's evaluation
    stack (the collector's scan surface). An executing bit per registered
    thread is the rendezvous denominator; Rule A (start executing) waits
    out an in-flight stop, Rule B (stop executing) arrives under it. The
    LAST arriver runs the collection and completes the epoch; waiters are
    released by the comparison "completedEpoch >= epoch" (epochs are a
    monotonic uint64, so a superseded epoch's waiters are released when
    the superseding epoch completes).

    Lock contract: the internal mutex is a LEAF lock — never acquire the
    GIL (or any other lock) while holding it. Acquiring it while holding
    the GIL is fine (GIL -> gcStop is the one legal order).
    WaitForCompletion and BeginExecuting must NOT be called while holding
    the GIL: waiters sleep on the condition variable, and an arrived
    party that needs the GIL to reach its safepoint would deadlock.

    Zombie tolerance: a detached carrier from a torn-down run (or a
    blocking-native callback spanning a Reset) may call Begin/EndExecuting
    after Reset retired its entry — or after a later registration reused
    its index. Both flips therefore verify the entry's generation against
    the stamp taken at THIS thread's registration (inside the leaf lock,
    atomic with retirement/reuse): a mismatch means the caller is in no
    denominator, and the flip silently reports false instead of touching
    the entry. Never-registered use still terminates (fail-loud).
---*/
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
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
    //--- K7: thread registry + executing-bit rendezvous ---

    //Register the calling thread at its OUTERMOST evaluation boundary
    //(carrier-loop entry, host-bridge outermost entry), publishing its
    //slot's evaluation stack — the vector lives in the thread's
    //carrier/fallback slot and is stable for the thread's lifetime.
    //Double registration on one instance is an outermost-rule violation
    //and trips the fail-loud terminate.
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
    //collector's scan of this thread's published stack. Returns false
    //for a zombie caller (retired/reused entry — in no denominator);
    //never-registered use terminates (fail-loud).
    bool BeginExecuting();

    //Rule B — call where the thread STOPS executing bytecode (park,
    //blocking-native entry, dispatch end). Flips the bit off; if a stop
    //is in flight, this transition IS the thread's arrival — flip and
    //arrival share one critical section, so a concurrent snapshot can
    //never miss either half. Returns true when this arrival completed
    //the expected set: the CALLER is the last arriver and must collect
    //(K8: lock-free — AssertAllArrived pins the quiescence) and call
    //Complete(). False = parked arrival or zombie (no arrival
    //happened — nothing to wait for).
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

    //K8: the collector's quiescence gate — call as the last arriver,
    //before the lock-free collection. Trips unless a stop is in flight
    //with every snapshot member arrived (the constructive fact behind
    //lock-free collection; a violation is an accounting bug, and
    //collecting without quiescence would race live bytecode).
    void AssertAllArrived();

    //Between runs: zeroes the epoch accounting and retires ALL entries
    //unconditionally (grace-period zombie carriers may still be
    //registered — an empty assert would fire on them). Retired entries
    //are marked dead IN PLACE: the deque never shrinks, so a detached
    //zombie's self-entry pointer stays valid (and inert).
    void Reset();

    //Telemetry: completed stop epochs — one count per Complete (each
    //collection-driven world stop, or a cancellation that released the
    //epoch's waiters). Zeroed by Reset with the epoch accounting.
    uint64_t CompletedStops() const {
        return m_stopCount.load(std::memory_order_acquire);
    }

    //Collector-side snapshot of the scan surface — the published
    //stacks of live registered threads, in registration order.
    std::vector<std::vector<CallFrame>*> RegisteredStacks() const;

    //Mark every live entry's published stack with the caller's
    //callback, holding the leaf lock for the WHOLE scan: any executing
    //flip must take that lock first, so "every entry is quiescent"
    //holds by construction (arrived threads block in WaitForCompletion,
    //non-executing threads are parked in ctx->frames or between
    //rendezvous steps). Do NOT "optimize" this into a snapshot-then-
    //scan — releasing the lock mid-scan reopens the race the wait
    //closes. The cost is latency (Rule A waiters queue on the lock),
    //never deadlock: nobody takes the leaf lock then the GIL.
    template <typename Fn>
    void ScanRegisteredStacks(Fn&& mark) {
        std::lock_guard<std::mutex> g(m_mutex);
        for (auto& entry : m_threads)
            if (entry.live.load(std::memory_order_relaxed))
                mark(*entry.evalStack);
    }

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
    //Zombie discriminator (leaf lock held): the entry this thread's
    //thread-locals point at still carries the generation stamped at
    //this thread's registration. Reset retires entries in place; index
    //reuse rewrites the generation — either way the stamp mismatches
    //and the caller is a zombie in no denominator.
    bool EntryIsCurrent(const ThreadEntry& entry) const {
        return entry.live.load(std::memory_order_relaxed)
            && entry.generation == t_selfGeneration;
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
    std::deque<ThreadEntry> m_threads;  //address-stable: never shrinks
    std::deque<size_t> m_freeIndices;   //retired slots, LIFO reuse
    uint64_t m_nextGeneration = 1;      //monotonic; 0 = never issued
    size_t m_executingCount = 0;        //registry threads with bit set
    std::atomic<bool> m_stopActive{false};  //stop in flight (fast poll)
    std::atomic<uint64_t> m_stopCount{0};   //completed epochs (telemetry)
    static thread_local GcWorldStop* t_selfStop;
    static thread_local ThreadEntry* t_selfEntry;
    static thread_local uint64_t t_selfGeneration;  //zombie stamp
};

} // namespace nlang
