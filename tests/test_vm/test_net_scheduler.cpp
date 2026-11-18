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
#include <future>
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

//Final review I1: neither half of the poller's registration may outlive the
//sleep that follows its last waiter. Before the fix WakeIoWaitersForFd (the
//close path) dropped the interest-table row and left the kernel entry behind,
//and the poller cv-sleeps once the table is empty, so the sweep that deletes
//vanished fds never ran to reap it. A socket later opened with the same fd
//value (the OS hands back the lowest free handle) then Mod-succeeded against
//that zombie — its interest landed on the dead socket's poll object — and its
//reader slept to the deadline instead of waking on traffic.
//TestCloseDropsKernelInterest pins the same window without the reuse lottery.
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

//The kernel-side half of the unregistration that TestCloseWakesDropsInterest
//pins for the table: dropping the row leaves the *kernel* entry behind,
//because wepoll keys its set by socket value and removes an entry only when it
//dequeues that socket's close completion. A poller round does pick it up within
//a slice, but only while the poller has a reason to round: when the close is the
//last interest the poller cv-sleeps again and the entry is never reaped at all.
//The next socket the OS opens with the same value — it hands back the lowest
//free handle, so a server that closes its listener and starts a new one comes
//back to it at once — then Mod-succeeds against that zombie: its interest lands
//on the dead socket's poll object, and the still-pending reap deletes that entry
//while the table keeps reporting it registered. Nothing re-registers it
//afterwards, so the reader is left with its deadline alone: the field case is an
//http accept loop stuck in accept(0) with a connection already in the listen
//backlog. The close path therefore has to delete the kernel entry itself — the
//table may never claim interest the kernel set lacks, the one skew a park cannot
//recover from on its own.
//
//BeginShutdownForTests stops the poller thread before the close, so the reap
//cannot race the reading: the thread leaves its wait without harvesting, which
//is what turns "the kernel still holds it" into a fact about the code rather
//than a bet on the machine (2026-11-14 ruling: no test assertion may depend on
//timing).
static void TestCloseDropsKernelInterest() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerKernelInterestForTests(fd, IO_EV_READ));   //the park registered it
    sched.BeginShutdownForTests();               //no reap can run behind us
    sched.WakeIoWaitersForFd(fd);                //the close path: last waiter
    CHECK(sched.IoPollerRegisteredForTests(fd) == 0);        //the row is gone…
    CHECK(!sched.IoPollerKernelInterestForTests(fd, IO_EV_READ));   //…and so is the kernel entry
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

//The interest table is the narrowing sweep's candidate list, not an authority a
//park may trust: every park re-declares what it needs, so an entry the kernel
//lost behind the table's back (the reap above, a delete that failed on a dying
//fd) is re-armed instead of being skipped as already registered. Trusting the
//row is what made the skew unrepairable: the row alone decided that the kernel
//still watched the fd, and no later park ever contradicted it.
static void TestStaleRowParkReRegisters() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerKernelInterestForTests(fd, IO_EV_READ));
    sched.IoPollerDropKernelInterestForTests(fd);      //the kernel forgot it…
    CHECK(sched.IoPollerRegisteredForTests(fd) == IO_EV_READ);   //…the table did not
    CHECK(!sched.IoPollerKernelInterestForTests(fd, IO_EV_READ));
    //The parked reader above still wants READ, so the narrowing sweep leaves the
    //row alone and the only writer of the kernel set is this park.
    const int again = sched.RegisterContext();
    sched.ParkForIo(again, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    CHECK(sched.IoPollerKernelInterestForTests(fd, IO_EV_READ));   //re-armed, not skipped
    sched.WakeIoWaitersForFd(fd);
    CHECK(WaitUntilRunnable(sched, id, 200));
    CHECK(WaitUntilRunnable(sched, again, 200));
    NetClose(fd);
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
    //A park that re-declares a mask the row already covers (r2 wants exactly
    //what a has) must still send the UNION to the kernel, not its own mask:
    //wepoll's sock_set_event replaces the watched set, so a bare READ here
    //would drop the write bit the parked writer waits for. Pin it with a fresh
    //context on the same fd.
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
    CHECK(WaitUntilRunnable(sched, r2, 200));          //the re-declaring park drains too
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
//such a round may never happen. `TestStaleRegistrationOnFdReuse` is what the
//invariant buys: a socket opened on a recycled fd value has to reach the fresh
//Add in `RegisterIoInterestLocked` instead of having its interest land on the
//previous socket's entry, where it would wait for its deadline alone.
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

//The close-side wake must hand the woken context to a carrier, the way every
//other direct wake does (WakeForMutex/WakeForSemaphore end with a carrier
//notify; the harvest and the timer sweep bulk-notify after their loops). The
//wake here came from a socket close, which can be the only wake an infinite
//deadline park ever gets — accept(0) has no deadline to fall back on — so a
//missing notify parks the connection loop until some unrelated event moves the
//queue. The state probe of the other tests cannot see this: the state flips and
//the context is enqueued either way, only nobody is told. So the assertion is
//made from a carrier's side: a carrier waiting on an empty queue must come back
//with the woken context.
//The poller is kept absent (the same injection switch TestHandleCreationFailure
//uses): its harvest round notifies the carriers after every round, which would
//cover for the close path's own notify and make the test pass for the wrong
//reason.
static void TestCloseWakeDispatchesCarrier() {
    VmScheduler sched;
    sched.SetIoPollerCreateFailsForTests(true);
    //Born parked so the run queue starts empty: in production a context parks
    //only after a dispatch took it out of the queue, and a queue entry the
    //carrier could pop would answer the question without the wake.
    const int ioId = sched.RegisterParkedPromiseContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(ioId, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    //White-box completion of the K7 park handshake (test_threads.cpp:3782):
    //a production park stores the frames at the park site, and a wake enqueues
    //only a context whose frames have landed.
    sched.ParkContextFrames(ioId, {});

    sched.ConfigureCarriers(1);
    std::promise<void> installed;
    auto started = installed.get_future();
    std::promise<int> popped;
    auto got = popped.get_future();
    std::thread carrier([&sched, &installed, &popped] {
        sched.InstallSelfCarrier(0);
        installed.set_value();                   //the slot exists before the wake
        popped.set_value(sched.PopNextRunnable());   //waits for the wake to enqueue
    });
    started.get();
    std::this_thread::sleep_for(50ms);         //the carrier is in its wait
    sched.WakeIoWaitersForFd(fd);               //the close path: last waiter
    const bool dispatched = got.wait_for(2s) == std::future_status::ready;
    CHECK(dispatched);                          //a carrier came back for it
    if (dispatched) {
        CHECK(got.get() == ioId);               //with the woken context
    } else {
        sched.RequestStop();                    //ends the carrier's wait: joinable
    }
    carrier.join();
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
    TestCloseDropsKernelInterest();
    TestStaleRowParkReRegisters();
    TestRegisterAtPark();
    TestUnionMaskAcrossParkSites();
    TestDeadlineOnlyPark();
    TestCloseWakesDropsInterest();
    TestCloseWakeDispatchesCarrier();
    TestHarvestNarrowsVanishedInterest();
    TestParkAfterShutdown();
    TestHandleCreationFailure();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
