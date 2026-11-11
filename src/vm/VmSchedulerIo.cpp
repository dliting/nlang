/*---
    VmSchedulerIo.cpp — IO parking concern of the context scheduler
    (v0.13.0 net): ParkForIo/WakeForIo, the interest table, and the
    dedicated lazy-started poller thread. The thread body mirrors
    TimerThreadMain (VmSchedulerTimer.cpp:100-130 — copy its cv/GIL
    discipline exactly): outside the GcWorldStop
    rendezvous by construction, never runs bytecode, holds the global
    lock only while mutating scheduler state — released around
    IoPollerWait (plan ruling R6 caps the wait so a newly registered
    interest is picked up within one slice).
---*/
#include "VmScheduler.h"
#include "VmWorkPool.h"
#include "IoPoller.h"
#include <nlang/runtime/StackDump.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nlang {

//v0.13.0 net: the poll slice (R6) — bounds the latency of picking up
//newly registered interest while the wait is fd-driven.
static constexpr int kIoPollSliceMs = 10;

//Out-of-line ctor (declaration in VmScheduler.h): the m_upWorkPool
//member's exception-cleanup path needs VmWorkPool complete, and this
//file includes it (VmScheduler.cpp is at the source-size budget).
VmScheduler::VmScheduler() = default;

//Unions the wanted mask with what this fd already has and makes the kernel
//match it. Add on first sight, Mod once it exists: an IOCP/wepoll Mod of a
//missing fd fails, so fall back to Add. Registration is what the poller
//thread's IoPollerWait observes; the table is the cross-park bookkeeping,
//and it may only UNDER-report the kernel set: the equal-mask shortcut below
//turns a stale entry into a skipped Add and strands the park on its
//deadline, while a missing entry self-heals through the Mod/Add pair.
int VmScheduler::RegisterIoInterestLocked(int fd, uint32_t events) {
    if (!m_upIoPoller || fd < 0) { return -1; }
    const auto it = m_ioPollerRegistered.find(fd);
    const uint32_t wanted = (it == m_ioPollerRegistered.end() ? 0 : it->second) | events;
    if (it != m_ioPollerRegistered.end() && it->second == wanted) { return 0; }
    void* poller = m_upIoPoller.get();
    int rc = IoPollerMod(poller, fd, wanted);
    if (rc != 0) { rc = IoPollerAdd(poller, fd, wanted); }
    if (rc == 0) { m_ioPollerRegistered[fd] = wanted; }
    return rc;
}

//D1's park-site lifecycle (GIL held): create the poller handle on the first
//park, union this fd's interest into the kernel set, and start the poller
//thread if it is missing — all before the context sleeps, so the poller
//thread is already blocking on this fd. The old shape (sync only when the
//interest table was empty) made the wake wait for the next loop iteration
//— the 15 ms step the ablation measurements located.
//A park racing shutdown touches the interest table and nothing else: no
//handle, no registration, no thread restart (assigning a new std::thread
//to the still-joinable m_ioThread would call std::terminate).
void VmScheduler::ArmIoPollerLocked(int fd, uint32_t events) {
    if (m_ioThreadShutdown) { return; }
    if (!m_upIoPoller) {
        //The injection flag is the test seam for the startup-order failure
        //side (spec 七): forcing the failure here is the only way to reach
        //the branch, since IoPollerCreate fails on OS resource exhaustion
        //only. The assert still fires for a real failure.
        void* created = m_ioPollerCreateFailsForTests ? nullptr : IoPollerCreate();
        if (!created) {
            NLANG_ASSERT(m_ioPollerCreateFailsForTests
                && "IoPollerCreate failed: no poller thread, parked IO wakes only on an explicit wake");
        } else {
            m_upIoPoller.reset(created);
        }
    }
    if (fd >= 0 && m_upIoPoller) {
        RegisterIoInterestLocked(fd, events);
    }
    //No handle (IoPollerCreate failed: OS resource exhaustion, asserted
    //above) means no thread either — a poller thread with a null handle
    //could not wait. Same dead end as the old early return in the thread.
    if (!m_ioThreadStarted && m_upIoPoller) {
        m_ioThreadStarted = true;
        m_ioThread = std::thread([this] { IoPollerThreadMain(); });
    }
    m_ioCv.notify_all();
}

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
    //D1: arm the poller here, while the GIL is held — see ArmIoPollerLocked.
    ArmIoPollerLocked(fd, events);
    ReleaseHeldIfCurrent(id);
    NotifyGateWaiters();
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
        //The fd is closing: every entry for it is gone, so its
        //baseline row must go too. The narrowing sweep alone cannot
        //guarantee it — the sweep runs only inside a harvest round,
        //and once the table is empty the poller cv-sleeps again, so
        //that round may never come; a later socket whose fd value the
        //OS re-uses would then match the stale row and have its Add
        //skipped by the equal-mask shortcut, stranding it on the
        //deadline (final review I1, now pinned by
        //TestCloseWakesDropsInterest). No kernel del: the registration
        //dies with the fd, so dropping the row costs nothing and keeps
        //the table honest ("may only under-report", VmScheduler.h).
        m_ioPollerRegistered.erase(fd);
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
//re-firing the same event every round (livelock guard, the
//ExpireDueTimers shape). WakeForIo is a no-op unless the context is
//still ParkedIo, so the erase is unconditional. The ready events are
//hashed by fd so one pass over the table harvests them all (the
//per-event table scans were O(evs × waits) at 10k parked sockets).
void VmScheduler::HarvestIoEvents(void* poller, std::vector<IoPollerEvent>& evs) {
    std::unordered_map<int, uint32_t> ready;
    for (const IoPollerEvent& ev : evs)
        ready[ev.fd] |= ev.events;
    for (auto it = m_ioWaits.begin(); it != m_ioWaits.end();) {
        const auto r = it->fd >= 0 ? ready.find(it->fd) : ready.end();
        const bool hit = r != ready.end()
            && ((it->events & r->second) != 0u
                || (r->second & (IO_EV_HUP | IO_EV_ERR)) != 0u);
        if (!hit) {
            ++it;
            continue;
        }
        WakeForIo(it->contextId);
        it = m_ioWaits.erase(it);
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
    //Narrowing sweep: anything the kernel still watches but no entry wants
    //any more must be deleted, or a closed connection keeps waking a carrier
    //forever. An fd that vanished with the kernel registration alive (the
    //handle-creation-failure case) is re-registered by its next park, so no
    //sticky-failure flag is needed here.
    std::unordered_map<int, uint32_t> wanted;
    for (const auto& e : m_ioWaits) {
        if (e.fd < 0) { continue; }
        wanted[e.fd] |= e.events;
    }
    for (auto it = m_ioPollerRegistered.begin(); it != m_ioPollerRegistered.end(); ) {
        const auto w = wanted.find(it->first);
        const uint32_t want = (w == wanted.end() ? 0 : w->second);
        if (want == 0) {
            IoPollerDel(poller, it->first);      //tolerates EBADF: the fd may be closed
            it = m_ioPollerRegistered.erase(it);
        } else if (it->second != want) {
            if (IoPollerMod(poller, it->first, want) == 0) { it->second = want; }
            ++it;
        } else {
            ++it;
        }
    }
}

//The poller thread body (lazy start, see ParkForIo — the handle is
//already created there, so the thread never creates or closes one).
//Empty interest: cv long sleep (releases the global lock). With
//interest: the park site has already registered it (D1), the wait
//timeout is the nearest deadline capped at kIoPollSliceMs, and the
//wait itself runs with the global lock RELEASED. Harvest only narrows
//the kernel set (deletes and shrinks); additions live at the park.
//Shutdown joins through ShutdownIoPollerThread's flag+notify (same
//lazy-thread protocol as the timer thread).
void VmScheduler::IoPollerThreadMain() {
    for (;;) {
        GilEnsureGuard gil(*this);
        m_ioCv.wait(gil.Lock(), [this] {
            return m_ioThreadShutdown || !m_ioWaits.empty();
        });
        if (m_ioThreadShutdown)
            break;
        const int timeoutMs = NextIoTimeoutMs();
        std::vector<IoPollerEvent> evs;       //thread-local scratch
        //Capture the raw pointer while the GIL is held; the handle is
        //reset only after this thread is joined (~VmScheduler), so it
        //cannot dangle inside this iteration.
        void* poller = m_upIoPoller.get();
        gil.Release();                       //wait without the GIL
        IoPollerWait(poller, evs, timeoutMs);
        GilEnsureGuard g2(*this);            //harvest under the GIL
        HarvestIoEvents(poller, evs);
        g2.Release();
        m_runQueueCv.notify_all();           //woken contexts: wake carriers
    }
}

//The io half of the destructor's shutdown (the timer half stays in
//~VmScheduler; the two threads never interact): set the flag under the
//GIL, wake the cv long sleep, join, and close the handle only once the
//thread is gone — the poller runs on the raw pointer it captured under
//the GIL (IoPollerThreadMain), so closing before the join would be a
//use-after-free inside IoPollerWait. With the thread never started the
//joinable check and the reset are both no-ops.
void VmScheduler::ShutdownIoPollerThread() {
    {
        GilEnsureGuard g(*this);
        m_ioThreadShutdown = true;   //v0.13.0 net: same lazy-thread protocol
    }
    m_ioCv.notify_all();
    if (m_ioThread.joinable())
        m_ioThread.join();
    m_upIoPoller.reset();
}

//D1 test probes: read the park-site registration contract (declarations
//in VmScheduler.h). Every one takes the GIL; tests call them from the
//main thread while the poller thread may be mid-iteration.
int VmScheduler::IoPollerRegisteredForTests(int fd) {
    GilEnsureGuard g(*this);
    const auto it = m_ioPollerRegistered.find(fd);
    return it == m_ioPollerRegistered.end() ? 0 : static_cast<int>(it->second);
}

int VmScheduler::IoPollerRegistrationCountForTests() {
    GilEnsureGuard g(*this);
    return static_cast<int>(m_ioPollerRegistered.size());
}

bool VmScheduler::IoPollerActiveForTests() {
    GilEnsureGuard g(*this);
    return static_cast<bool>(m_upIoPoller);
}

bool VmScheduler::IoThreadStartedForTests() {
    GilEnsureGuard g(*this);
    return m_ioThreadStarted;
}

void VmScheduler::BeginShutdownForTests() {
    //The flag+notify half of ShutdownIoPollerThread: the poller has to leave
    //its cv wait to see the flag, so the notify is part of the shape, not an
    //extra. Leaving the join (and the handle close) to the destructor is the
    //point — that is the "flag set, thread not yet joined" window the test
    //asserts in.
    GilEnsureGuard g(*this);
    m_ioThreadShutdown = true;
    m_ioCv.notify_all();      //WakeIoWaitersForFd:97 notifies inside the guard too
}

void VmScheduler::SetIoPollerCreateFailsForTests(bool fails) {
    GilEnsureGuard g(*this);
    m_ioPollerCreateFailsForTests = fails;
}

} // namespace nlang
