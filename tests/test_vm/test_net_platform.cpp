/*---
    test_net_platform.cpp — the OS socket abstraction contract: loopback
    TCP/UDP round trips, the poller set, numeric resolution, and the
    unprivileged ICMP echo. No VM, no scheduler.
---*/
#include "net/NetPlatform.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace nlang;   //the brief's test body calls the entries unqualified

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static void TestTcpLoopbackRoundTrip() {
    CHECK(NetPlatformInit());
    int ls = NetOpenTcpStream(NetFamily::Inet);
    CHECK(ls >= 0);
    CHECK(NetBind(ls, "127.0.0.1", 0) == 0);
    CHECK(NetListen(ls, 8) == 0);
    const int port = NetLocalPort(ls);
    CHECK(port > 0);
    //Client connects; accept on the listener must report readable.
    //Non-blocking contract (NetPlatform.h): 0 now or kNetInProgress,
    //judged later by NetConnectResult.
    int cli = NetOpenTcpStream(NetFamily::Inet);
    const int crc = NetConnect(cli, "127.0.0.1", port);
    CHECK(crc == 0 || crc == kNetInProgress);
    if (crc == kNetInProgress) {
        int verdict = -1;
        for (int spin = 0; spin < 10000 && (verdict = NetConnectResult(cli)) != 0; ++spin)
            ;
        CHECK(verdict == 0);
    }
    int srv = -1;
    for (int spin = 0; spin < 1000 && (srv = NetAccept(ls)) < 0; ++spin)
        ;   //loopback connect is immediate; bounded spin, no sleep
    CHECK(srv >= 0);
    const uint8_t msg[3] = { 10, 20, 30 };
    CHECK(NetSend(cli, msg, 3) == 3);
    uint8_t got[3] = { 0, 0, 0 };
    int n = -1;
    for (int spin = 0; spin < 1000 && ((n = NetRecv(srv, got, 3)) < 0); ++spin)
        ;
    CHECK(n == 3);
    CHECK(got[0] == 10 && got[1] == 20 && got[2] == 30);
    //EOF after peer shutdown.
    NetShutdownWrite(cli);
    NetClose(cli);
    n = -1;
    for (int spin = 0; spin < 10000 && ((n = NetRecv(srv, got, 3)) > 0); ++spin)
        ;
    CHECK(n == 0);   //EOF
    NetClose(srv);
    NetClose(ls);
}

static void TestWouldBlock() {
    int ls = NetOpenTcpStream(NetFamily::Inet);
    CHECK(NetBind(ls, "127.0.0.1", 0) == 0);
    CHECK(NetListen(ls, 1) == 0);
    CHECK(NetAccept(ls) == kNetWouldBlock);   //empty backlog
    NetClose(ls);
}

static void TestUdpRoundTrip() {
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    CHECK(NetBind(a, "127.0.0.1", 0) == 0);
    CHECK(NetBind(b, "127.0.0.1", 0) == 0);
    const uint8_t msg[2] = { 7, 8 };
    CHECK(NetSendTo(a, msg, 2, "127.0.0.1", NetLocalPort(b)) == 2);
    uint8_t got[4] = { 0, 0, 0, 0 };
    std::string from;
    int fromPort = 0;
    int n = -1;
    for (int spin = 0; spin < 10000 && ((n = NetRecvFrom(b, got, 4, from, fromPort)) < 0); ++spin)
        ;
    CHECK(n == 2);
    CHECK(from == "127.0.0.1");
    CHECK(fromPort == NetLocalPort(a));
    NetClose(a);
    NetClose(b);
}

static void TestPoller() {
    void* p = NetPollerCreate();
    CHECK(p != nullptr);
    int a = NetOpenUdp(NetFamily::Inet), b = NetOpenUdp(NetFamily::Inet);
    NetBind(a, "127.0.0.1", 0);
    NetBind(b, "127.0.0.1", 0);
    CHECK(NetPollerAdd(p, b, NET_EV_READ) == 0);
    std::vector<NetPollEvent> evs;
    CHECK(NetPollerWait(p, evs, 10) == 0);        //no traffic: empty
    CHECK(evs.empty());
    const uint8_t m[1] = { 1 };
    NetSendTo(a, m, 1, "127.0.0.1", NetLocalPort(b));
    CHECK(NetPollerWait(p, evs, 2000) > 0);
    CHECK(!evs.empty() && evs[0].fd == b && (evs[0].events & NET_EV_READ));
    NetPollerDel(p, b);
    NetClose(a);
    NetClose(b);
    NetPollerClose(p);
}

static void TestResolveAndIcmp() {
    std::vector<NetEndpoint> eps;
    CHECK(NetResolve("localhost", 80, eps));
    CHECK(!eps.empty());
    //Unprivileged echo (IcmpSendEcho2 / DGRAM ICMP): loopback answers.
    const uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    const int rtt = NetIcmpEcho("127.0.0.1", payload, 4, 3000);
    CHECK(rtt >= 0);   //-1 timeout, -2 error — loopback must answer
}

int main() {
    TestTcpLoopbackRoundTrip();
    TestWouldBlock();
    TestUdpRoundTrip();
    TestPoller();
    TestResolveAndIcmp();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
