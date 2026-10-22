/*---
    VmSchedulerTimer.cpp — timer-thread concern of the context scheduler
    (K2 slice 3): sleep parking, expiry migration, and the dedicated lazy
    thread. Split from VmScheduler.cpp (K5 task 6) to keep both files
    under the source-size guard.
---*/
#include "VmScheduler.h"
#include <algorithm>

namespace nlang {

//Sleep parking (slice 7): record the state and wake deadline into the
//timer table; the current context yields its carrier. Frame relocation
//is done by the carrier at the ParkSignal capture site. K2 slice 3: the
//first registration lazily starts the timer thread (resident for the
//executor's lifetime afterwards); every registration wakes it to
//recompute the nearest deadline.
void VmScheduler::ParkForTimer(int id,
                               std::chrono::steady_clock::time_point wakeAt) {
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    ctx->state = ContextState::ParkedTimer;
    ctx->wakeAt = wakeAt;
    m_timers.push_back(TimerEntry{id, wakeAt});
    ReleaseHeldIfCurrent(id);
    //K4: focus parked — release gate waiters (m_timerCv belongs to the
    //timer thread and would not reach them).
    NotifyGateWaiters();
    if (!m_timerThreadStarted) {
        m_timerThreadStarted = true;
        m_timerThread = std::thread([this] { TimerThreadMain(); });
    }
    m_timerCv.notify_all();
}

//Expiry migration (see the header). The linear table's size equals the
//number of parked sleep contexts — heap structure unnecessary at K1
//scale. Stale entries (state no longer ParkedTimer) must be dropped:
//keeping one would make PopNextRunnable spin forever on a deadline
//already past (livelock). K2 slice 3: expired contexts enqueue sorted by
//expiry time — when the servicing side arrives late (the timer thread
//starved off the GIL, or a slow cold start) several deadlines may have
//passed at once, and wake order must follow expiry time rather than
//registration order (the two orders are unrelated).
void VmScheduler::ExpireDueTimers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<TimerEntry> remaining;
    std::vector<TimerEntry> due;
    remaining.reserve(m_timers.size());
    for (const TimerEntry& t : m_timers) {
        ExecutionContext* ctx = FindMutable(t.contextId);
        if (!ctx || ctx->state != ContextState::ParkedTimer)
            continue;   //stale — dropped, never re-armed
        if (t.wakeAt <= now)
            due.push_back(t);
        else
            remaining.push_back(t);
    }
    std::stable_sort(due.begin(), due.end(),
        [](const TimerEntry& a, const TimerEntry& b) {
            return a.wakeAt < b.wakeAt;
        });
    for (const TimerEntry& t : due) {
        ExecutionContext* ctx = FindMutable(t.contextId);
        ctx->state = ContextState::Runnable;
        m_runQueue.push_back(ctx->id);
    }
    m_timers = std::move(remaining);
}

std::chrono::steady_clock::time_point VmScheduler::NearestTimerDeadline() const {
    auto nearest = m_timers.front().wakeAt;
    for (const TimerEntry& t : m_timers)
        if (t.wakeAt < nearest)
            nearest = t.wakeAt;
    return nearest;
}

//K2 slice 3: the timer thread body (lazily started, see ParkForTimer).
//The outer wait is for "timers exist to service"; the inner one sleeps
//to the nearest deadline. Waking midway on any of three changes —
//shutdown, the table emptied, or a newly registered earlier deadline
//uniformly detected as a nearest-deadline change — recomputes and
//continues; otherwise expired contexts migrate and the carriers wake.
//If every stale entry was dropped with nothing enqueued, the loop falls
//back to the outer wait (under the current semantics a parked sleep
//context's state cannot change; the stale branch is defensive only).
void VmScheduler::TimerThreadMain() {
    for (;;) {
        //K5 batch 1: time this acquisition like the carrier guards do —
        //the timer thread is a real GIL contender the guard-only
        //counters never saw. The cv waits below reacquire the lock
        //internally; those reacquisitions stay uncounted (same ruling
        //as ServiceDueTimers in the header).
        const auto gilWaitStartedAt = BeginGilWait();
        std::unique_lock<std::mutex> gil(m_gil, std::defer_lock);
        gil.lock();
        EndGilWait(gilWaitStartedAt);
        m_timerCv.wait(gil, [this] {
            return m_timerShutdown || !m_timers.empty();
        });
        if (m_timerShutdown)
            return;
        const auto nearest = NearestTimerDeadline();
        const bool deadlineReached = !m_timerCv.wait_until(gil, nearest,
            [this, &nearest] {
                return m_timerShutdown || m_timers.empty()
                    || NearestTimerDeadline() != nearest;
            });
        if (m_timerShutdown)
            return;
        if (!deadlineReached)
            continue;   //table changed: recompute the nearest deadline
        ExpireDueTimers();
        gil.unlock();
        m_runQueueCv.notify_all();   //expired contexts enqueued: wake carriers
    }
}

} // namespace nlang
