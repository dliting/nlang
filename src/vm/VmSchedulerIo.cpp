/*---
    VmSchedulerIo.cpp — IO parking concern of the context scheduler
    (v0.13.0 net): ParkForIo/WakeForIo, the interest table, and the
    dedicated lazy-started poller thread. The thread body mirrors
    TimerThreadMain (VmSchedulerTimer.cpp:100-130 — copy its cv/GIL
    discipline exactly): outside the GcWorldStop
    rendezvous by construction, never runs bytecode, holds the global
    lock only while mutating scheduler state — released around
    NetPollerWait (plan ruling R6 caps the wait so a newly registered
    interest is picked up within one slice).
---*/
#include "VmScheduler.h"
#include "VmWorkPool.h"
#include "net/NetPlatform.h"
#include <algorithm>
#include <memory>
#include <vector>

namespace nlang {

//v0.13.0 net: the poll slice (R6) — bounds the latency of picking up
//newly registered interest while the wait is fd-driven.
static constexpr int kIoPollSliceMs = 10;

//Out-of-line ctor (declaration in VmScheduler.h): the m_upWorkPool
//member's exception-cleanup path needs VmWorkPool complete, and this
//file includes it (VmScheduler.cpp is at the source-size budget).
VmScheduler::VmScheduler() = default;

void VmScheduler::ParkForIo(int id, int fd, uint32_t events,
    std::chrono::steady_clock::time_point deadline, bool hasDeadline) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx)
        return;
    //v11 R7: the blocking-native park path arrives with state
    //BlockedNative — MarkBlockedNative counted it, and MarkRunnableHeld
    //(the only decrement site) is skipped by the park unwind, so
    //release the count here at the transition: it stays equal to the
    //number of BlockedNative contexts by construction (deadlock
    //detection reads it).
    if (ctx->state == ContextState::BlockedNative)
        --m_blockedNativeCount;
    ctx->state = ContextState::ParkedIo;
    m_ioWaits.push_back(IoWaitEntry{id, fd, events, deadline, hasDeadline});
    ReleaseHeldIfCurrent(id);
    NotifyGateWaiters();
    if (!m_ioThreadStarted) {
        m_ioThreadStarted = true;
        m_ioThread = std::thread([this] { IoPollerThreadMain(); });
    }
    m_ioCv.notify_all();
}

void VmScheduler::WakeForIo(int id) {
    GilEnsureGuard g(*this);
    ExecutionContext* ctx = FindMutable(id);
    if (!ctx || ctx->state != ContextState::ParkedIo)
        return;
    ctx->state = ContextState::Runnable;
    //v11 R7: arm the blocking-native re-entry — the re-executed
    //parkForIo call answers 1 (retry the syscall). Interpreter-path
    //parks never read the flag; a stale armed is harmless (contract in
    //NativeHost.h).
    ctx->ioParkArmed = true;
    if (ctx->framesParked)
        m_runQueue.push_back(ctx->id);
    //else: the parking carrier enqueues in the same critical section
    //that stores the frames (K7 handshake, same as WakeForMutex).
}

void VmScheduler::RemoveIoWaitsForFd(int fd) {
    GilEnsureGuard g(*this);
    m_ioWaits.erase(
        std::remove_if(m_ioWaits.begin(), m_ioWaits.end(),
            [fd](const IoWaitEntry& e) { return e.fd == fd; }),
        m_ioWaits.end());
    m_ioCv.notify_all();   //the poller re-syncs interest without it
}

//v11 R13: the net module's socket-close path (ABI wakeIoWaiters). The
//waiter ids and the interest entries are dropped in one critical
//section (the poller re-syncs without them), then each context is
//woken — WakeForIo self-guards, so the wakes run outside the section.
void VmScheduler::WakeIoWaitersForFd(int fd) {
    std::vector<int> ids;
    {
        GilEnsureGuard g(*this);
        for (auto it = m_ioWaits.begin(); it != m_ioWaits.end(); ) {
            if (it->fd == fd) {
                ids.push_back(it->contextId);
                it = m_ioWaits.erase(it);
            } else {
                ++it;
            }
        }
        m_ioCv.notify_all();
    }
    for (int id : ids)
        WakeForIo(id);
}

//r1 note 4: ABI v11 submitWork accessor (declaration in VmScheduler.h;
//placed here — VmScheduler.cpp is at the source-size budget and this
//file already owns the lazy helper-thread concern). The pool's own
//destructor joins it during ~VmScheduler member teardown (R8).
int VmScheduler::SubmitWork(void (*fn)(void*), void* arg,
                            int ackFd, int priority) {
    std::lock_guard<std::mutex> g(m_workPoolMutex);
    if (!m_upWorkPool)
        m_upWorkPool = std::make_unique<VmWorkPool>();
    m_upWorkPool->Submit(fn, arg, ackFd, priority);
    return 0;
}

//Re-syncs the poller registration set to the current interest table:
//per-fd wanted events are aggregated (several contexts may wait on one
//socket), registrations equal to m_ioPollerRegistered are skipped (the
//per-slice re-sync costs no syscalls for a steady set), the rest go
//through Add (fresh fd) or Mod (changed events), and fds that vanished
//from the table are del'd. Add on an already-registered fd fails with
//EEXIST, so Mod is the fallback — the pair is idempotent either way.
void VmScheduler::SyncPollerInterest(void* poller) {
    std::vector<std::pair<int, uint32_t>> wanted;
    for (const IoWaitEntry& e : m_ioWaits) {
        if (e.fd < 0)
            continue;   //deadline-only park: no poll interest
        bool found = false;
        for (auto& w : wanted)
            if (w.first == e.fd) { w.second |= e.events; found = true; break; }
        if (!found)
            wanted.emplace_back(e.fd, e.events);
    }
    for (const auto& w : wanted) {
        bool unchanged = false;
        for (const auto& r : m_ioPollerRegistered)
            if (r.first == w.first && r.second == w.second) {
                unchanged = true;
                break;
            }
        if (unchanged)
            continue;
        if (NetPollerAdd(poller, w.first, w.second) != 0)
            NetPollerMod(poller, w.first, w.second);
    }
    for (auto it = m_ioPollerRegistered.begin();
         it != m_ioPollerRegistered.end();) {
        bool still = false;
        for (const auto& w : wanted)
            if (w.first == it->first) { still = true; break; }
        if (still) {
            ++it;
        } else {
            NetPollerDel(poller, it->first);
            it = m_ioPollerRegistered.erase(it);
        }
    }
    m_ioPollerRegistered = wanted;   //next loop's dedup baseline
}

//Wait timeout: the nearest deadline across entries capped at the poll
//slice (R6), floored at 0; no deadline entries → the bare slice.
int VmScheduler::NextIoTimeoutMs() const {
    auto nearest = std::chrono::steady_clock::time_point::max();
    bool any = false;
    for (const IoWaitEntry& e : m_ioWaits)
        if (e.hasDeadline && e.deadline < nearest) {
            nearest = e.deadline;
            any = true;
        }
    if (!any)
        return kIoPollSliceMs;
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        nearest - std::chrono::steady_clock::now()).count();
    if (ms < 0)
        return 0;
    return ms < kIoPollSliceMs ? static_cast<int>(ms) : kIoPollSliceMs;
}

//Wakes everything the wait reported ready, then everything whose
//deadline passed. An event matches an entry when the fd matches and
//the entry wants a reported bit — HUP/ERR count as ready for every
//direction (the fd is dead either way). Woken and stale entries are
//both removed: a context that left ParkedIo on its own must not keep
//re-firing the same event every slice (livelock guard, the
//ExpireDueTimers shape). WakeForIo is a no-op unless the context is
//still ParkedIo, so the erase is unconditional.
void VmScheduler::HarvestIoEvents(void*, std::vector<NetPollEvent>& evs) {
    for (const NetPollEvent& ev : evs) {
        for (auto it = m_ioWaits.begin(); it != m_ioWaits.end();) {
            const bool hit = it->fd == ev.fd
                && ((it->events & ev.events) != 0u
                    || (ev.events & (NET_EV_HUP | NET_EV_ERR)) != 0u);
            if (!hit) {
                ++it;
                continue;
            }
            WakeForIo(it->contextId);
            it = m_ioWaits.erase(it);
        }
    }
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_ioWaits.begin(); it != m_ioWaits.end();) {
        if (it->hasDeadline && !(now < it->deadline)) {
            WakeForIo(it->contextId);
            it = m_ioWaits.erase(it);
        } else {
            ++it;
        }
    }
}

//The poller thread body (lazy start, see ParkForIo). Empty interest:
//cv long sleep (releases the global lock). With interest: interest is
//re-synced to the poller set each loop (idempotent add/mod), the wait
//timeout is the nearest deadline capped at kIoPollSliceMs, and the
//wait itself runs with the global lock RELEASED. Shutdown joins
//through the destructor's flag+notify (same lazy-thread protocol as
//the timer thread).
void VmScheduler::IoPollerThreadMain() {
    void* poller = NetPollerCreate();
    for (;;) {
        GilEnsureGuard gil(*this);
        m_ioCv.wait(gil.Lock(), [this] {
            return m_ioThreadShutdown || !m_ioWaits.empty();
        });
        if (m_ioThreadShutdown)
            break;
        SyncPollerInterest(poller);
        const int timeoutMs = NextIoTimeoutMs();
        std::vector<NetPollEvent> evs;       //thread-local scratch
        gil.Release();                       //wait without the GIL
        NetPollerWait(poller, evs, timeoutMs);
        GilEnsureGuard g2(*this);            //harvest under the GIL
        HarvestIoEvents(poller, evs);
        g2.Release();
        m_runQueueCv.notify_all();           //woken contexts: wake carriers
    }
    NetPollerClose(poller);
}

} // namespace nlang
