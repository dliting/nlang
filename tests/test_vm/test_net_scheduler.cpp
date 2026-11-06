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

int main() {
    Runtime::StaticInit();
    TestPoller();
    TestWakeOnReadiness();
    TestWakeOnDeadline();
    TestRemoveOnClose();
    TestStaleRegistrationOnFdReuse();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
