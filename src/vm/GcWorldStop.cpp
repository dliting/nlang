/*---
    GcWorldStop.cpp — GC stop-the-world rendezvous (K6 protocol +
    K7 thread registry)
---*/
#include "GcWorldStop.h"
#include <cstdio>

namespace nlang {
//Per-thread evaluation-window depth. Static (not per-instance): nested
//windows of DIFFERENT executors on one thread would double-count into
//either instance's total; the codebase never stacks two executors'
//RunFrames on one thread (the embedding host drives one executor at a
//time per thread), and the count the protocol needs is per-scheduler
//anyway — noted here so the assumption is visible, not accidental.
thread_local int GcWorldStop::t_evalDepth = 0;
//Parked-window depth saves (LIFO — see SuspendEvalWindow's contract).
thread_local std::vector<int> GcWorldStop::t_suspendedDepths;
//Registry self-entry (same one-executor-per-thread assumption as
//t_evalDepth; see SelfEntry's contract in the header).
thread_local GcWorldStop* GcWorldStop::t_selfStop = nullptr;
thread_local GcWorldStop::ThreadEntry* GcWorldStop::t_selfEntry = nullptr;

//--- K7: thread registry + executing-bit rendezvous ---

void GcWorldStop::Tripwire(const char* what)
{
    std::fprintf(stderr,
        "GcWorldStop protocol violation: %s (unregistered or wrong"
        " state) — terminating\n", what);
    std::terminate();
}

GcThreadToken GcWorldStop::RegisterThread(std::vector<CallFrame>* evalStack)
{
    std::lock_guard<std::mutex> g(m_mutex);
    ThreadEntry* self = SelfEntry();
    if (self != nullptr && self->live.load(std::memory_order_relaxed))
        Tripwire("RegisterThread (already registered)");
    GcThreadToken token;
    token.generation = m_nextGeneration++;
    if (m_freeIndices.empty()) {
        token.index = m_threads.size();
        m_threads.emplace_back();   //address-stable: never relocated
    } else {
        token.index = m_freeIndices.back();
        m_freeIndices.pop_back();
    }
    ThreadEntry& entry = m_threads[token.index];
    entry.evalStack = evalStack;
    entry.executing.store(false, std::memory_order_relaxed);
    entry.live.store(true, std::memory_order_relaxed);
    entry.generation = token.generation;
    t_selfStop = this;
    t_selfEntry = &entry;
    return token;
}

void GcWorldStop::UnregisterThread(GcThreadToken token)
{
    std::lock_guard<std::mutex> g(m_mutex);
    if (token.generation == 0 || token.index >= m_threads.size())
        return;
    ThreadEntry& entry = m_threads[token.index];
    if (!entry.live.load(std::memory_order_relaxed)
        || entry.generation != token.generation)
        return;   //stale token (Reset retire or index reuse) — no-op
    if (entry.executing.load(std::memory_order_relaxed))
        Tripwire("UnregisterThread (still executing)");
    entry.live.store(false, std::memory_order_relaxed);
    m_freeIndices.push_back(token.index);
    if (SelfEntry() == &entry)
        t_selfEntry = nullptr;
}

bool GcWorldStop::IsRegistered() const
{
    const ThreadEntry* self =
        const_cast<GcWorldStop*>(this)->SelfEntry();
    return self != nullptr
        && self->live.load(std::memory_order_relaxed);
}

bool GcWorldStop::ExecutingSelf() const
{
    const ThreadEntry* self =
        const_cast<GcWorldStop*>(this)->SelfEntry();
    return self != nullptr
        && self->live.load(std::memory_order_relaxed)
        && self->executing.load(std::memory_order_relaxed);
}

void GcWorldStop::BeginExecuting()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    ThreadEntry* self = SelfEntry();
    if (self == nullptr || !self->live.load(std::memory_order_relaxed))
        Tripwire("BeginExecuting");
    //Rule A: park until no stop is in flight, THEN flip on — the
    //collector scans the published stacks of non-executing threads, so
    //an executing flip during a stop would race the scan. The wait
    //sleeps on the cv; the caller holds no global lock (contract).
    m_cv.wait(lk,
        [this] { return !m_stopActive.load(std::memory_order_relaxed); });
    ++m_executingCount;
    self->executing.store(true, std::memory_order_relaxed);
}

bool GcWorldStop::EndExecuting()
{
    std::lock_guard<std::mutex> g(m_mutex);
    ThreadEntry* self = SelfEntry();
    if (self == nullptr || !self->live.load(std::memory_order_relaxed))
        Tripwire("EndExecuting");
    if (!self->executing.load(std::memory_order_relaxed))
        Tripwire("EndExecuting (not executing)");
    self->executing.store(false, std::memory_order_relaxed);
    --m_executingCount;
    //Rule B: with a stop in flight, this stop-executing transition is
    //the thread's arrival (same critical section as the flip above).
    if (!m_stopActive.load(std::memory_order_relaxed))
        return false;
    ++m_arrived;
    return m_arrived == m_expected;
}

bool GcWorldStop::InitiateAndArrive()
{
    std::lock_guard<std::mutex> g(m_mutex);
    ThreadEntry* self = SelfEntry();
    if (self == nullptr || !self->live.load(std::memory_order_relaxed)
        || !self->executing.load(std::memory_order_relaxed))
        Tripwire("InitiateAndArrive");
    if (!m_stopActive.load(std::memory_order_relaxed)) {
        //Sole initiator: advance the epoch and snapshot the executing
        //count (self included); the self-arrival below lands in the
        //same critical section, so no observer can see the snapshot
        //and the arrival separately.
        ++m_requestEpoch;
        m_stopActive.store(true, std::memory_order_release);
        m_expected = m_executingCount;
        m_arrived = 0;
    }
    //else: dual initiation — join the in-flight stop as an arrival;
    //a second epoch is never advanced, so the K6 single-flight
    //terminate tripwire has no successor here.
    self->executing.store(false, std::memory_order_relaxed);
    --m_executingCount;
    ++m_arrived;
    return m_arrived == m_expected;
}

void GcWorldStop::WaitForCompletion()
{
    std::unique_lock<std::mutex> lk(m_mutex);
    const uint64_t epoch = m_requestEpoch;   //observed at entry
    m_cv.wait(lk, [this, epoch] { return m_completedEpoch >= epoch; });
}

void GcWorldStop::Complete()
{
    {
        std::lock_guard<std::mutex> g(m_mutex);
        if (m_requestEpoch > m_completedEpoch)
            m_completedEpoch = m_requestEpoch;
        m_stopActive.store(false, std::memory_order_release);
    }
    m_cv.notify_all();
}

void GcWorldStop::Reset()
{
    std::lock_guard<std::mutex> g(m_mutex);
    m_requestEpoch = 0;
    m_completedEpoch = 0;
    m_expected = 0;
    m_arrived = 0;
    m_executingCount = 0;
    m_stopActive.store(false, std::memory_order_release);
    //Retire every entry IN PLACE — the deque never shrinks, so a
    //detached zombie carrier's t_selfEntry stays a valid pointer to an
    //inert entry. All slots become reusable; the generation counter
    //keeps advancing, so tokens issued before the Reset can never
    //match a later registration (even one reusing the same index).
    for (auto& entry : m_threads)
        entry.live.store(false, std::memory_order_relaxed);
    m_freeIndices.clear();
    for (size_t i = m_threads.size(); i-- > 0; )
        m_freeIndices.push_back(i);
}

std::vector<std::vector<CallFrame>*>
GcWorldStop::RegisteredStacks() const
{
    std::lock_guard<std::mutex> g(m_mutex);
    std::vector<std::vector<CallFrame>*> roots;
    for (auto& entry : m_threads)
        if (entry.live.load(std::memory_order_relaxed))
            roots.push_back(entry.evalStack);
    return roots;
}

} // namespace nlang
