/*---
    test_net_scheduler.cpp — the IO parking contract: the poller set
    itself (register/wait/deliver), and a parked context waking on fd
    readiness, on deadline expiry, and on an explicit close-side wake;
    every wake keeps the framesParked handshake (the woken context is
    enqueued exactly once).
---*/
#include "VmScheduler.h"
#include "IoPoller.h"
#include "net/net_platform.h"
#include <nlang/runtime/Runtime.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

using namespace nlang;
using namespace std::chrono;

//Drives the scheduler single-threaded like a carrier: register a
//context, park it, then poll the context state for the wake. (Do
//NOT poll PopNextRunnable here: it returns an int context id and
//BLOCKS on an empty queue with deadlock detection — VmScheduler.h
//300/294-305. The test context never parked real frames, so
//WakeForIo only flips state to Runnable without enqueuing.)
static bool WaitUntilRunnable(VmScheduler& sched, int id, int spins) {
    for (int spin = 0; spin < spins; ++spin) {
        if (sched.Find(id)->state == ContextState::Runnable)
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

static void TestPoller() {
    void* p = IoPollerCreate();
    CHECK(p != nullptr);
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    CHECK(IoPollerAdd(p, b, IO_EV_READ) == 0);
    std::vector<IoPollerEvent> evs;
    CHECK(IoPollerWait(p, evs, 10) == 0);         //no traffic: empty
    CHECK(evs.empty());
    const uint8_t m[1] = { 1 };
    NetSendTo(a, m, 1, "127.0.0.1", NetLocalPort(b));
    CHECK(IoPollerWait(p, evs, 2000) > 0);
    CHECK(!evs.empty() && evs[0].fd == b && (evs[0].events & IO_EV_READ));
    IoPollerDel(p, b);
    NetClose(a);
    NetClose(b);
    IoPollerClose(p);
}

static void TestWakeOnReadiness() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    sched.ParkForIo(id, b, IO_EV_READ, steady_clock::now() + 5s, true);
    CHECK(sched.Find(id)->state == ContextState::ParkedIo);
    //Traffic from another OS thread: the poller must wake the context.
    std::thread peer([&] {
        const uint8_t m[1] = { 1 };
        std::this_thread::sleep_for(50ms);   //park registered
        NetSendTo(a, m, 1, "127.0.0.1", NetLocalPort(b));
    });
    const bool woke = WaitUntilRunnable(sched, id, 2000);
    peer.join();
    CHECK(woke);
    NetClose(a);
    NetClose(b);
}

static void TestWakeOnDeadline() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 100ms, true);
    const auto t0 = steady_clock::now();
    CHECK(WaitUntilRunnable(sched, id, 4000));
    CHECK(steady_clock::now() - t0 >= 90ms);   //not an early wake
    NetClose(fd);
}

//Final review I1: the poller's registration baseline must not outlive
//the sleep that follows its last waiter. WakeIoWaitersForFd (the
//close path) empties the interest table, the poller thread cv-sleeps
//before its del-vanished-fds loop can run, and the baseline keeps
//(fd, events). A socket later opened with the same fd value (the OS
//hands back the lowest free handle) is then skipped by the
//equal-registration check — the kernel registration died with the
//old fd — and its reader sleeps to the deadline instead of waking on
//traffic.
static void TestStaleRegistrationOnFdReuse() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int peer = NetOpenUdp(NetFamily::Inet);
    NetBind(peer, "127.0.0.1", 0);

    //Park on a socket, close-wake it (the last waiter: the table
    //empties), close it, and reopen until the OS re-uses the value.
    int closed = -1, fd = -1;
    for (int attempt = 0; attempt < 64 && fd < 0; ++attempt) {
        const int sock = NetOpenUdp(NetFamily::Inet);
        NetBind(sock, "127.0.0.1", 0);
        if (sock == closed) {
            fd = sock;                 //the value came back: stale window
            break;
        }
        closed = sock;
        sched.ParkForIo(id, sock, IO_EV_READ, steady_clock::now() + 30s, true);
        sched.WakeIoWaitersForFd(sock);   //the close path: last waiter
        CHECK(WaitUntilRunnable(sched, id, 200));
        NetClose(sock);
    }
    CHECK(fd >= 0);   //the OS re-used a handle value within the budget
    if (fd < 0) {
        NetClose(peer);
        return;
    }
    //fd is live but unregistered (pre-fix): a reader must still wake
    //on traffic, well before the deadline.
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 2s, true);
    std::thread sender([&] {
        const uint8_t m[1] = { 7 };
        std::this_thread::sleep_for(50ms);   //park registered
        NetSendTo(peer, m, 1, "127.0.0.1", NetLocalPort(fd));
    });
    const auto t0 = steady_clock::now();
    const bool woke = WaitUntilRunnable(sched, id, 2000);
    sender.join();
    CHECK(woke);
    CHECK(steady_clock::now() - t0 < 1500ms);   //readiness, not deadline
    NetClose(fd);
    NetClose(peer);
}

static void TestRemoveOnClose() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    sched.WakeIoWaitersForFd(fd);   //the close path: unregister + wake
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

//D1: the interest must exist the moment a context parks, not only at the
//next poller-loop iteration. Before the fix the poller slept on an empty
//set and the wake arrived at the next 15 ms tick instead of on readiness.
static void TestRegisterAtPark() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    CHECK(!sched.IoPollerActiveForTests());      //no poller before the first park
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoThreadStartedForTests());
    CHECK(sched.IoPollerRegisteredForTests(fd) == IO_EV_READ);   //registered at the park
    sched.WakeIoWaitersForFd(fd);
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

//Two parks on one fd with different masks (a reader then a writer): the
//kernel interest is the UNION, and neither park may lose the other's bit.
static void TestUnionMaskAcrossParkSites() {
    VmScheduler sched;
    const int r = sched.RegisterContext(), w = sched.RegisterContext();
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    sched.ParkForIo(r, a, IO_EV_READ,  steady_clock::now() + 30s, true);
    sched.ParkForIo(w, a, IO_EV_WRITE, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerRegisteredForTests(a) == (IO_EV_READ | IO_EV_WRITE));
    //A park re-declaring the union mask the kernel already has for a takes
    //the equal-mask shortcut in RegisterIoInterestLocked: no Add/Mod
    //round-trip. This is the branch the erase in WakeIoWaitersForFd guards:
    //without it the table over-reports, and a later Add/Mod on an fd the
    //kernel has dropped fails (EEXIST on POSIX Add, ERROR_INVALID_HANDLE on
    //IOCP), so pin the shortcut with a fresh context on the same fd.
    const int r2 = sched.RegisterContext();
    sched.ParkForIo(r2, a, IO_EV_READ | IO_EV_WRITE, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerRegisteredForTests(a) == (IO_EV_READ | IO_EV_WRITE));
    //r parks a second time, on the new fd b: its entry on a from the first
    //park stays live (the explicit Wake(a) below drains it), and
    //registering b is a fresh Add. b doubles as the sender socket.
    sched.ParkForIo(r, b, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerRegisteredForTests(b) == IO_EV_READ);
    const uint8_t m[1] = { 1 };
    NetSendTo(b, m, 1, "127.0.0.1", NetLocalPort(a));   //b is writable, a is not readable
    CHECK(WaitUntilRunnable(sched, w, 200));            //the union kept the write bit alive
    sched.WakeIoWaitersForFd(a);
    sched.WakeIoWaitersForFd(b);
    CHECK(WaitUntilRunnable(sched, r, 200));
    CHECK(WaitUntilRunnable(sched, r2, 200));          //the equal-mask park drains too
    NetClose(a);
    NetClose(b);
}

//A deadline-only park (fd = -1: the wait-for-nothing sleep) must still arm
//the poller thread — a parked context's deadline is evaluated only inside
//HarvestIoEvents, so with no thread nobody wakes it — and must register
//nothing. The count probe is what makes "nothing" observable: the fd-keyed
//probe alone cannot tell an empty set from an unrelated fd.
static void TestDeadlineOnlyPark() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    sched.ParkForIo(id, -1, 0, steady_clock::now() + 100ms, true);
    CHECK(sched.IoThreadStartedForTests());
    CHECK(sched.IoPollerRegistrationCountForTests() == 0);
    const auto t0 = steady_clock::now();
    CHECK(WaitUntilRunnable(sched, id, 4000));
    CHECK(steady_clock::now() - t0 >= 90ms);
}

//D1 unregistration via the close path (spec §七 "deletion-side invariants + a
//close-path case"): the net module's socket close (`ABI wakeIoWaiters`) drops
//the registration when the last waiter disappears. The registration must be
//gone by the time the call returns — the narrowing sweep only runs inside
//harvest rounds, and an empty table lets the poller thread cv-sleep again, so
//such a round may never happen. This invariant is exactly what
//`TestStaleRegistrationOnFdReuse` (`:102`) relies on: `RegisterIoInterestLocked`
//takes its equal-mask shortcut over a stale (fd, events) as if the kernel still
//had it registered and skips the Add, leaving a new socket to wait for its
//deadline alone.
static void TestCloseWakesDropsInterest() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerRegisteredForTests(fd) == IO_EV_READ);
    sched.WakeIoWaitersForFd(fd);                 //the close path: last waiter
    CHECK(sched.IoPollerRegisteredForTests(fd) == 0);   //gone on return, not one round later
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

//D1 unregistration via the harvest path: a harvest-internal wake (here a due
//deadline) erases that fd's interest in the same round while another parked fd
//must keep its registration. The old "sweep only when the table is empty"
//shape fired only when no context waited anywhere; the narrowed sweep is
//D1-new code that no other test would cover. The assertions are race-free:
//`WakeForIo` (`VmSchedulerIo.cpp:200`) flips the state and sweeps inside the
//same GIL section, and the test's `Find` needs the GIL, so it can only run
//after the poller thread passes `g2.Release()` (`:251`) — by then the sweep has
//already finished.
static void TestHarvestNarrowsVanishedInterest() {
    VmScheduler sched;
    const int due = sched.RegisterContext(), held = sched.RegisterContext();
    int fdDue = NetOpenUdp(NetFamily::Inet), fdHeld = NetOpenUdp(NetFamily::Inet);
    NetBind(fdDue, "127.0.0.1", 0);
    NetBind(fdHeld, "127.0.0.1", 0);
    sched.ParkForIo(held, fdHeld, IO_EV_READ, steady_clock::now() + 30s, true);
    sched.ParkForIo(due, fdDue, IO_EV_READ, steady_clock::now() + 100ms, true);
    CHECK(sched.IoPollerRegisteredForTests(fdDue) == IO_EV_READ);
    CHECK(sched.IoPollerRegisteredForTests(fdHeld) == IO_EV_READ);
    CHECK(WaitUntilRunnable(sched, due, 4000));   //deadline wake, one harvest round
    CHECK(sched.IoPollerRegisteredForTests(fdDue) == 0);      //swept in that round
    CHECK(sched.IoPollerRegisteredForTests(fdHeld) == IO_EV_READ);   //narrowing, not clearing
    sched.WakeIoWaitersForFd(fdHeld);
    CHECK(WaitUntilRunnable(sched, held, 200));
    NetClose(fdDue);
    NetClose(fdHeld);
}

//D1 shutdown invariant (spec §七): a park landing in the window where the
//shutdown flag is set but the poller thread is not yet joined must neither
//register interest nor start a thread — assigning a new `std::thread` to the
//still-joinable `m_ioThread` would `std::terminate`, uncovered today. A first
//normal park brings the thread up, the shutdown flag is set, and the second
//park falls into that window. There is no "deadline would still wake"
//assertion: an IO park's deadline is only evaluated in harvest, and after
//shutdown the poller thread exits with nobody watching it (`TestWakeOnDeadline`
//wakes only because its poller thread stays alive).
static void TestParkAfterShutdown() {
    VmScheduler sched;
    const int first = sched.RegisterContext(), late = sched.RegisterContext();
    int fd1 = NetOpenUdp(NetFamily::Inet), fd2 = NetOpenUdp(NetFamily::Inet);
    NetBind(fd1, "127.0.0.1", 0);
    NetBind(fd2, "127.0.0.1", 0);
    sched.ParkForIo(first, fd1, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoThreadStartedForTests());       //a live poller before shutdown
    sched.BeginShutdownForTests();                //the destructor's flag+notify half
    sched.ParkForIo(late, fd2, IO_EV_READ, steady_clock::now() + 200ms, true);
    //Reaching the checks at all is the std::terminate assertion: restarting the
    //poller would assign to the still-joinable m_ioThread and abort the binary.
    CHECK(sched.IoPollerRegisteredForTests(fd2) == 0);        //no interest after shutdown
    CHECK(sched.IoPollerRegistrationCountForTests() == 1);    //still exactly fd1's
    CHECK(sched.Find(late)->state == ContextState::ParkedIo); //nothing can wake it now
    //The handle outlives the exited thread: it is closed only after the join
    //(Step 6), so the poller's captured pointer cannot dangle.
    CHECK(sched.IoPollerActiveForTests());
    sched.WakeIoWaitersForFd(fd1);                //parked contexts are not woken by ~VmScheduler
    CHECK(WaitUntilRunnable(sched, first, 200));
    NetClose(fd1);
    NetClose(fd2);
}

//D1 startup-order failure side (spec §七 "the handle-creation-failure side is
//verified by assertion or test hook to not start the thread"): `IoPollerCreate`
//fails only under system-resource exhaustion — a normally unreachable branch —
//so an injection switch manufactures it. No handle means no thread: a poller
//thread holding a null handle cannot report readiness, equivalent to the old
//shape where the thread exits at once. The in-code `NLANG_ASSERT` is
//conditioned on the injection switch being off, so an injected failure does
//not abort an assertion build; on a genuine failure the switch reads false and
//the assertion stands as usual.
static void TestHandleCreationFailure() {
    VmScheduler sched;
    sched.SetIoPollerCreateFailsForTests(true);
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(!sched.IoPollerActiveForTests());                  //no handle was made
    CHECK(!sched.IoThreadStartedForTests());                 //so no thread either
    CHECK(sched.Find(id)->state == ContextState::ParkedIo);  //the entry is still recorded
    sched.WakeIoWaitersForFd(fd);              //an explicit wake works without a poller
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

int main() {
    Runtime::StaticInit();
    TestPoller();
    TestWakeOnReadiness();
    TestWakeOnDeadline();
    TestRemoveOnClose();
    TestStaleRegistrationOnFdReuse();
    TestRegisterAtPark();
    TestUnionMaskAcrossParkSites();
    TestDeadlineOnlyPark();
    TestCloseWakesDropsInterest();
    TestHarvestNarrowsVanishedInterest();
    TestParkAfterShutdown();
    TestHandleCreationFailure();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
