/*---
    net_platform.h — the OS socket abstraction of the net package
    (net library design §6). One header, two implementations
    (net_platform_windows.cpp / net_platform_posix.cpp). All sockets
    are non-blocking; "no data / no space" surfaces as kNetWouldBlock,
    an in-flight connect as kNetInProgress. Blocking conveniences
    (NetResolve, NetIcmpEcho) run only on the host's work-pool threads
    (ABI submitWork) — never on a carrier. The IO poller set is a VM
    facility (IoPoller.h), not part of this layer.

    Error convention: -1 = failure with NetLastErrorText() carrying a
    human-readable OS message; kNetWouldBlock / kNetInProgress are
    non-failure status codes. A fd is a plain OS socket handle.
---*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Non-failure status codes returned where -1 means "failed".
constexpr int kNetWouldBlock = -1000;
constexpr int kNetInProgress = -1001;

//Address family. The neutral header keeps its own enum: AF_INET6 is
//23 on Windows but 10 on Linux/10 on macOS, so callers must not pass
//raw AF_* values; each platform TU maps this enum to its AF_*.
enum class NetFamily { Inet, Inet6 };

//One resolved endpoint: numeric address text + port + family
//(spec §三: IPv6 literals and dual-stack listen ship in v1; sockets
//open with the endpoint's family).
struct NetEndpoint {
    std::string address;
    int port = 0;
    NetFamily family = NetFamily::Inet;
};

//Process-level socket subsystem init (WSAStartup on Windows).
//Idempotent through a once flag (plan ruling R4); every net entry
//point calls it first. First init registers atexit(WSACleanup) for
//the host exit path (spec §6).
bool NetPlatformInit();

//--- TCP -----------------------------------------------------------------
int NetOpenTcpStream(NetFamily family);
//port 0 = auto; addr "::" → dual-stack listener (IPV6_V6ONLY=0 set
//before bind, IPv4 peers arrive v4-mapped).
int NetBind(int fd, const std::string& addr, int port);
int NetListen(int fd, int backlog);
//Non-blocking: 0 = connected now, kNetInProgress = in flight (judge
//later with NetConnectResult), kNetWouldBlock = backlog empty,
//-1 = failed.
int NetConnect(int fd, const std::string& addr, int port);
int NetAccept(int fd);
//0 = peer closed (EOF). Stream send returns bytes written (may be <
//len; the caller loops).
int NetRecv(int fd, uint8_t* dst, int len);
int NetSend(int fd, const uint8_t* src, int len);
int NetShutdownWrite(int fd);
//Post-kNetInProgress verdict: 0 = ok, kNetInProgress = still in flight
//(park again — a park wake can be spurious), -1 = failed (reason in
//NetLastErrorText). SO_ERROR alone cannot tell "in progress" from "ok"
//(a pending connect also reports no error), so the verdict first probes
//writability with a zero timeout: only a finished connect — ok or
//failed — is writable.
int NetConnectResult(int fd);

//--- UDP -----------------------------------------------------------------
int NetOpenUdp(NetFamily family);
int NetRecvFrom(int fd, uint8_t* dst, int len,
                std::string& outAddress, int& outPort);
int NetSendTo(int fd, const uint8_t* src, int len,
              const std::string& addr, int port);

//--- shared socket state --------------------------------------------------
void NetClose(int fd);
int NetLocalPort(int fd);
std::string NetLocalAddress(int fd);
int NetPeerPort(int fd);
//v4-mapped IPv6 endpoints print as plain dotted IPv4
//(IN6_IS_ADDR_V4MAPPED → unwrap), so dual-stack listeners report
//IPv4 peers the way the script layer bound them.
std::string NetPeerAddress(int fd);
bool NetSetNoDelay(int fd, bool on);
bool NetSetKeepAlive(int fd, bool on);
bool NetSetSendBuffer(int fd, int bytes);
bool NetSetRecvBuffer(int fd, int bytes);
bool NetSetBroadcast(int fd, bool on);
//imr_interface = the socket's bound address (INADDR_ANY when
//wildcard/unbound) so a self-join + loopback send delivers the loop
//copy on the same interface the send leaves by. IPv6 groups use
//IPV6_JOIN_GROUP with the bound v6 address (or 0).
bool NetJoinMulticast(int fd, const std::string& group);
bool NetLeaveMulticast(int fd, const std::string& group);
bool NetSetMulticastTtl(int fd, int ttl);
bool NetSetTtl(int fd, int ttl);

//--- socket pair (the job self-pipe substrate, plan ruling R9) ------------
//Creates a connected, non-blocking, close-on-exec socket pair in
//fds[0]/fds[1]. Windows: a loopback TCP pair (the ack channel must be
//pollable and wepoll only knows sockets — R9); POSIX: socketpair.
//Returns 0 on success, -1 on failure.
int NetSocketPair(int fds[2]);

//--- blocking conveniences (worker threads only) ---------------------------
//Numeric-literal fast path (inet_pton AF_INET then AF_INET6,
//stripping surrounding brackets — "[::1]" form, spec D10) over
//getaddrinfo(AF_UNSPEC); fills endpoints of both families in resolver
//order. connect tries them in order (v6-first stacks still fall back
//to v4 when the listener is v4-only).
bool NetResolve(const std::string& host, int port,
                std::vector<NetEndpoint>& out);
//True for a numeric IPv4/IPv6 literal (bracketed "[::1]" accepted) —
//the _tcpResolveStart fast-path gate: a literal fills its job record
//on the spot instead of paying a pool round trip. Same test as
//NetResolve's own numeric path (inet_pton).
bool NetIsNumericAddress(const std::string& host);
//ICMP echo round trip: returns RTT milliseconds, -1 = timeout,
//-2 = unreachable/error. Resolves first; prefers the first IPv4
//endpoint (IcmpSendEcho2, unprivileged), else the first IPv6
//(Icmp6SendEcho2 with source "::"). POSIX skeleton: DGRAM ICMP.
int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len,
                int timeoutMs);

//--- raw ICMP sockets (IcmpSocket) -----------------------------------------
//Windows: SOCK_RAW/IPPROTO_ICMP (admin; bind reports the admin
//requirement through NetLastErrorText). POSIX: SOCK_DGRAM/IPPROTO_ICMP.
//v1 scope: IPv4 raw only (AF_INET); IPv6 echo goes through
//NetIcmpEcho (ping), not through IcmpSocket.
int NetOpenIcmp();
int NetIcmpSend(int fd, const uint8_t* msg, int len,
                const std::string& addr);
int NetIcmpRecv(int fd, uint8_t* dst, int len, std::string& outAddress);

//Human-readable last OS error (WSAGetLastError / errno mapped).
std::string NetLastErrorText();

}  //namespace nlang
