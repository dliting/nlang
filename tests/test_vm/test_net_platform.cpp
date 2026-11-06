/*---
    test_net_platform.cpp — the OS socket abstraction contract: loopback
    TCP/UDP round trips, numeric resolution, and the unprivileged ICMP
    echo. No VM, no scheduler (the poller set contract lives in
    test_net_scheduler.cpp).
---*/
#include "net/net_platform.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipexport.h>   //IP_SUCCESS / IP_TIMEOUT / IP_DEST_UNREACHABLE
#else
#include <cerrno>
#endif

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

static void TestResolveAndIcmp() {
    std::vector<NetEndpoint> eps;
    CHECK(NetResolve("localhost", 80, eps));
    CHECK(!eps.empty());
    //Unprivileged echo (IcmpSendEcho2 / DGRAM ICMP): loopback answers.
    const uint8_t payload[4] = { 'p', 'i', 'n', 'g' };
    const int rtt = NetIcmpEcho("127.0.0.1", payload, 4, 3000);
    CHECK(rtt >= 0);   //-1 timeout, -2 error — loopback must answer
}

static void TestIcmpStatusVerdict() {
    //External review I5: the ICMP failure classification is a pure
    //function so the unreachable-vs-timeout split is pinned without a
    //live network (the loopback echo above can produce neither).
#ifdef _WIN32
    CHECK(NetIcmpStatusVerdict(IP_SUCCESS) == 0);
    CHECK(NetIcmpStatusVerdict(IP_REQ_TIMED_OUT) == -1);
    CHECK(NetIcmpStatusVerdict(IP_DEST_HOST_UNREACHABLE) == -2);
    CHECK(NetIcmpStatusVerdict(IP_GENERAL_FAILURE) == -2);
#else
    CHECK(NetIcmpStatusVerdict(0) == 0);
    CHECK(NetIcmpStatusVerdict(EAGAIN) == -1);
    CHECK(NetIcmpStatusVerdict(EHOSTUNREACH) == -2);
    CHECK(NetIcmpStatusVerdict(ENETUNREACH) == -2);
#endif
}

static void TestSendWouldBlockVerdict() {
    //External review I6: ENOBUFS — the documented "output buffer full"
    //error of datagram sends — must read as would-block so the UDP
    //send natives park and retry instead of failing with -4. Loopback
    //never fills a send buffer, so the mapping is pinned as a pure
    //function (the park loop mirrors the proven TCP write shape).
#ifdef _WIN32
    CHECK(NetErrWouldBlock(WSAEWOULDBLOCK));
    CHECK(NetErrWouldBlock(WSAENOBUFS));
    CHECK(!NetErrWouldBlock(WSAECONNREFUSED));
    CHECK(!NetErrWouldBlock(0));
#else
    CHECK(NetErrWouldBlock(EWOULDBLOCK));
    CHECK(NetErrWouldBlock(EAGAIN));
    CHECK(NetErrWouldBlock(ENOBUFS));
    CHECK(!NetErrWouldBlock(ECONNREFUSED));
    CHECK(!NetErrWouldBlock(0));
#endif
}

int main() {
    TestTcpLoopbackRoundTrip();
    TestWouldBlock();
    TestUdpRoundTrip();
    TestResolveAndIcmp();
    TestIcmpStatusVerdict();
    TestSendWouldBlockVerdict();
    std::printf(g_failures ? "FAILED %d\n" : "ok\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
