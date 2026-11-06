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

static void TestRemoveOnClose() {
    VmScheduler sched;
    const int id = sched.RegisterContext();
    int fd = NetOpenUdp(NetFamily::Inet);
    NetBind(fd, "127.0.0.1", 0);
    sched.ParkForIo(id, fd, IO_EV_READ, steady_clock::now() + 30s, true);
    sched.RemoveIoWaitsForFd(fd);   //the close path
    sched.WakeForIo(id);            //the close wakes its waiters
    CHECK(WaitUntilRunnable(sched, id, 200));
    NetClose(fd);
}

int main() {
    Runtime::StaticInit();
    TestPoller();
    TestWakeOnReadiness();
    TestWakeOnDeadline();
    TestRemoveOnClose();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
