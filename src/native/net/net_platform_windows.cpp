/*---
    net_platform_windows.cpp — Winsock2 implementation of net_platform.h
    (net library design §6), core sockets + resolver. Sockets are
    non-blocking; the IO poller set is a VM facility (IoPoller.h /
    IoPollerWindows.cpp), not part of this layer. Blocking conveniences
    (NetResolve, NetIcmpEcho) are worker-thread only. Shared Winsock
    helpers live in net_platform_internal.h; the ICMP entries live in
    net_platform_icmp_windows.cpp.
---*/
#include "net_platform_internal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nlang {
namespace {

std::once_flag g_wsaOnce;
bool g_wsaOk = false;   //written inside the call_once, read after it
                        //(call_once supplies the happens-before edge)

void WsaInitOnce() {
    WSADATA data;
    g_wsaOk = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (g_wsaOk)
        std::atexit([] { WSACleanup(); });   //paired with the once (plan ruling R4)
}

//Multicast join/leave for both families; interface = the socket's bound
//address (wildcard/any when unbound) so the loop copy arrives (spec §三).
bool MulticastGroupOp(int fd, const std::string& group, bool join) {
    sockaddr_storage ss;
    int len = 0;
    if (!MakeAddr(group, 0, ss, len)) return false;
    SOCKET s = Fd(fd);
    if (ss.ss_family == AF_INET) {
        ip_mreq mreq;
        mreq.imr_multiaddr = reinterpret_cast<sockaddr_in*>(&ss)->sin_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        sockaddr_storage bound;
        int boundLen = 0;
        if (QueryName(fd, false, bound, boundLen)
            && bound.ss_family == AF_INET)
            mreq.imr_interface = reinterpret_cast<sockaddr_in*>(&bound)->sin_addr;
        return setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                          reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0;
    }
    ipv6_mreq mreq6;
    mreq6.ipv6mr_multiaddr = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr;
    //v6 interface index 0 = "any": the bound-address form needs an index
    //lookup; loopback-scoped v6 groups (spec §三) work with 0.
    mreq6.ipv6mr_interface = 0;
    return setsockopt(s, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                      reinterpret_cast<const char*>(&mreq6), sizeof(mreq6)) == 0;
}

}  //namespace

bool NetPlatformInit() {
    std::call_once(g_wsaOnce, WsaInitOnce);
    return g_wsaOk;
}

int NetOpenTcpStream(NetFamily family) {
    NetPlatformInit();
    const SOCKET s = socket(ToAf(family), SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    SetNonBlocking(s);
    return ToFd(s);
}

int NetOpenUdp(NetFamily family) {
    NetPlatformInit();
    const SOCKET s = socket(ToAf(family), SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    SetNonBlocking(s);
    return ToFd(s);
}

int NetBind(int fd, const std::string& addr, int port) {
    sockaddr_storage ss;
    int len = 0;
    if (!MakeAddr(addr, port, ss, len)) return -1;
    if (ss.ss_family == AF_INET6 && AddrText(reinterpret_cast<sockaddr*>(&ss)) == "::")
        SetSockOptInt(Fd(fd), IPPROTO_IPV6, IPV6_V6ONLY, 0);   //dual-stack before bind
    if (bind(Fd(fd), reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        const int err = WSAGetLastError();
        int type = 0, size = sizeof(type);   //admin denial is the raw-ICMP case
        if ((err == WSAEACCES || err == WSAEADDRNOTAVAIL)
            && getsockopt(Fd(fd), SOL_SOCKET, SO_TYPE,
                          reinterpret_cast<char*>(&type), &size) == 0
            && type == SOCK_RAW)
            g_lastErrorOverride = "raw ICMP requires administrator privileges on Windows";
        return -1;
    }
    return 0;
}

int NetListen(int fd, int backlog) {
    return listen(Fd(fd), backlog) == 0 ? 0 : -1;
}

int NetConnect(int fd, const std::string& addr, int port) {
    sockaddr_storage ss;
    int len = 0;
    if (!MakeAddr(addr, port, ss, len)) return -1;
    if (connect(Fd(fd), reinterpret_cast<sockaddr*>(&ss), len) == 0) return 0;
    const int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEALREADY || err == WSAEINPROGRESS)
        return kNetInProgress;
    return -1;
}

int NetAccept(int fd) {
    const SOCKET c = accept(Fd(fd), nullptr, nullptr);
    if (c == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return kNetWouldBlock;
        return -1;
    }
    SetNonBlocking(c);
    return ToFd(c);
}

int NetRecv(int fd, uint8_t* dst, int len) {
    const int n = recv(Fd(fd), reinterpret_cast<char*>(dst), len, 0);
    if (n > 0) return n;
    if (n == 0) return 0;   //EOF
    if (WSAGetLastError() == WSAEWOULDBLOCK) return kNetWouldBlock;
    return -1;
}

int NetSend(int fd, const uint8_t* src, int len) {
    const int n = send(Fd(fd), reinterpret_cast<const char*>(src), len, 0);
    if (n >= 0) return n;
    if (WSAGetLastError() == WSAEWOULDBLOCK) return kNetWouldBlock;
    return -1;
}

int NetShutdownWrite(int fd) {
    return shutdown(Fd(fd), SD_SEND) == 0 ? 0 : -1;
}

int NetConnectResult(int fd) {
    //Zero-timeout completion probe (see net_platform.h): a pending
    //connect also reports SO_ERROR == 0, so a stale park wake must not
    //mint a half-open stream — only a finished connect is writable.
    //Windows reports the two outcomes on different sets: success on
    //writables, failure on exceptfds (the canonical non-blocking
    //connect idiom). Watching writables alone made a refused connect
    //look "still in progress" forever: the poller keeps re-firing the
    //latched connect-failure event, and the park/verdict loop spins
    //until the deadline (the 10k-connection benchmark stall).
    fd_set wfds, efds;
    FD_ZERO(&wfds);
    FD_SET(Fd(fd), &wfds);
    FD_ZERO(&efds);
    FD_SET(Fd(fd), &efds);
    timeval tv = { 0, 0 };
    if (select(0, nullptr, &wfds, &efds, &tv) <= 0)
        return kNetInProgress;   //neither set ready: still connecting
    int err = 0;
    int size = sizeof(err);
    if (getsockopt(Fd(fd), SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&err), &size) != 0)
        return -1;
    if (err == 0)
        //Writables-ready = handshake done. exceptfds-ready with
        //SO_ERROR == 0 is a spurious exception some AFD builds
        //report: judge it still in flight, not failed.
        return FD_ISSET(Fd(fd), &wfds) ? 0 : kNetInProgress;
    char buf[128] = { 0 };
    std::snprintf(buf, sizeof(buf), "connect failed: socket error %d", err);
    g_lastErrorOverride = buf;
    return -1;
}

int NetRecvFrom(int fd, uint8_t* dst, int len,
                std::string& outAddress, int& outPort) {
    sockaddr_storage ss;
    int ssLen = sizeof(ss);
    const int n = recvfrom(Fd(fd), reinterpret_cast<char*>(dst), len, 0,
                           reinterpret_cast<sockaddr*>(&ss), &ssLen);
    if (n < 0) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return kNetWouldBlock;
        return -1;
    }
    outAddress = AddrText(reinterpret_cast<sockaddr*>(&ss));
    outPort = AddrPort(reinterpret_cast<sockaddr*>(&ss));
    return n;
}

int NetSendTo(int fd, const uint8_t* src, int len,
              const std::string& addr, int port) {
    sockaddr_storage ss;
    int ssLen = 0;
    if (!MakeAddr(addr, port, ss, ssLen)) return -1;
    const int n = sendto(Fd(fd), reinterpret_cast<const char*>(src), len, 0,
                         reinterpret_cast<sockaddr*>(&ss), ssLen);
    if (n >= 0) return n;
    if (WSAGetLastError() == WSAEWOULDBLOCK) return kNetWouldBlock;
    return -1;
}

void NetClose(int fd) {
    closesocket(Fd(fd));
}

int NetLocalPort(int fd) {
    sockaddr_storage ss;
    int len = 0;
    return QueryName(fd, false, ss, len) ? AddrPort(reinterpret_cast<sockaddr*>(&ss)) : -1;
}

std::string NetLocalAddress(int fd) {
    sockaddr_storage ss;
    int len = 0;
    return QueryName(fd, false, ss, len) ? AddrText(reinterpret_cast<sockaddr*>(&ss))
                                         : std::string();
}

int NetPeerPort(int fd) {
    sockaddr_storage ss;
    int len = 0;
    return QueryName(fd, true, ss, len) ? AddrPort(reinterpret_cast<sockaddr*>(&ss)) : -1;
}

std::string NetPeerAddress(int fd) {
    sockaddr_storage ss;
    int len = 0;
    return QueryName(fd, true, ss, len) ? AddrText(reinterpret_cast<sockaddr*>(&ss))
                                        : std::string();
}

bool NetSetNoDelay(int fd, bool on) {
    return SetSockOptInt(Fd(fd), IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

bool NetSetKeepAlive(int fd, bool on) {
    return SetSockOptInt(Fd(fd), SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}

bool NetSetSendBuffer(int fd, int bytes) {
    return SetSockOptInt(Fd(fd), SOL_SOCKET, SO_SNDBUF, bytes);
}

bool NetSetRecvBuffer(int fd, int bytes) {
    return SetSockOptInt(Fd(fd), SOL_SOCKET, SO_RCVBUF, bytes);
}

bool NetSetBroadcast(int fd, bool on) {
    return SetSockOptInt(Fd(fd), SOL_SOCKET, SO_BROADCAST, on ? 1 : 0);
}

bool NetJoinMulticast(int fd, const std::string& group) {
    return MulticastGroupOp(fd, group, true);
}

bool NetLeaveMulticast(int fd, const std::string& group) {
    return MulticastGroupOp(fd, group, false);
}

bool NetSetMulticastTtl(int fd, int ttl) {
    return SetSockOptInt(Fd(fd), IPPROTO_IP, IP_MULTICAST_TTL, ttl);
}

bool NetSetTtl(int fd, int ttl) {
    //the v4 knob is IP_TTL; the v6 equivalent is the unicast hop limit
    //(the posix layer branches the same way — final review M12)
    sockaddr_storage ss;
    int len = 0;
    if (QueryName(fd, false, ss, len) && ss.ss_family == AF_INET6)
        return SetSockOptInt(Fd(fd), IPPROTO_IPV6, IPV6_UNICAST_HOPS, ttl);
    return SetSockOptInt(Fd(fd), IPPROTO_IP, IP_TTL, ttl);
}

int NetSocketPair(int fds[2]) {
    NetPlatformInit();
    fds[0] = fds[1] = -1;
    //Loopback TCP pair: bind an ephemeral listener, connect to it,
    //accept. The connect/accept run blocking — loopback completes the
    //handshake in the kernel backlog, so neither call can stall — and
    //both ends flip to non-blocking afterwards (self-pipe contract).
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return -1;
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof(addr);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), len) != 0
        || listen(ls, 1) != 0
        || getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        closesocket(ls);
        return -1;
    }
    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (cli == INVALID_SOCKET) { closesocket(ls); return -1; }
    if (connect(cli, reinterpret_cast<sockaddr*>(&addr), len) != 0) {
        closesocket(cli); closesocket(ls); return -1;
    }
    SOCKET srv = accept(ls, nullptr, nullptr);
    closesocket(ls);
    if (srv == INVALID_SOCKET) { closesocket(cli); return -1; }
    SetNonBlocking(cli);
    SetNonBlocking(srv);
    //Windows sockets are non-inheritable by default, so the
    //close-on-exec contract holds without extra work.
    fds[0] = ToFd(srv);   //read end (the side the poller watches)
    fds[1] = ToFd(cli);   //write end (the ack byte goes here)
    return 0;
}

bool NetResolve(const std::string& host, int port, std::vector<NetEndpoint>& out) {
    NetPlatformInit();
    out.clear();
    std::string text = host;
    if (!text.empty() && text.front() == '[' && text.back() == ']')
        text = text.substr(1, text.size() - 2);
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        out.push_back({ text, port, NetFamily::Inet });
        return true;
    }
    in6_addr v6;
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        out.push_back({ text, port, NetFamily::Inet6 });
        return true;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* list = nullptr;
    if (getaddrinfo(text.c_str(), nullptr, &hints, &list) != 0) return false;
    for (addrinfo* ai = list; ai; ai = ai->ai_next) {
        char buf[INET6_ADDRSTRLEN] = { 0 };
        if (ai->ai_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr,
                      buf, sizeof(buf));
            out.push_back({ buf, port, NetFamily::Inet });
        } else if (ai->ai_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr,
                      buf, sizeof(buf));
            out.push_back({ buf, port, NetFamily::Inet6 });
        }
    }
    freeaddrinfo(list);
    return !out.empty();
}

bool NetIsNumericAddress(const std::string& host) {
    std::string text = host;
    if (!text.empty() && text.front() == '[' && text.back() == ']')
        text = text.substr(1, text.size() - 2);
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1)
        return true;
    in6_addr v6;
    return inet_pton(AF_INET6, text.c_str(), &v6) == 1;
}

std::string NetLastErrorText() {
    if (!g_lastErrorOverride.empty()) {
        std::string text = g_lastErrorOverride;
        g_lastErrorOverride.clear();
        return text;
    }
    const DWORD err = WSAGetLastError();
    wchar_t buf[256] = { 0 };
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, 255, nullptr);
    char utf8[512] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 511, nullptr, nullptr);
    while (utf8[0] && (utf8[strlen(utf8) - 1] == '\n' || utf8[strlen(utf8) - 1] == '\r'))
        utf8[strlen(utf8) - 1] = 0;
    return utf8;
}

}  //namespace nlang
