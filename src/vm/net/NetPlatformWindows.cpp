/*---
    NetPlatformWindows.cpp — Winsock2 implementation of NetPlatform.h
    (net library design §6), core sockets + poller + resolver. Sockets
    are non-blocking; the poller set is vendored wepoll
    (src/3rdparty/wepoll, BSD-2-Clause). Blocking conveniences
    (NetResolve, NetIcmpEcho) are worker-thread only. Shared Winsock
    helpers live in NetPlatformInternal.h; the ICMP entries live in
    NetPlatformWindowsIcmp.cpp.
---*/
#include "net/NetPlatformInternal.h"

#include <windows.h>

#include "wepoll.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nlang {
namespace {

std::once_flag g_wsaOnce;

void WsaInitOnce() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    std::atexit([] { WSACleanup(); });   //paired with the once (plan ruling R4)
}

uint32_t ToEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & NET_EV_READ) e |= EPOLLIN | EPOLLRDNORM;
    if (events & NET_EV_WRITE) e |= EPOLLOUT | EPOLLWRNORM;
    return e;
}

uint32_t FromEpollEvents(uint32_t events) {
    uint32_t e = 0;
    if (events & (EPOLLIN | EPOLLRDNORM | EPOLLRDHUP)) e |= NET_EV_READ;
    if (events & (EPOLLOUT | EPOLLWRNORM)) e |= NET_EV_WRITE;
    if (events & EPOLLHUP) e |= NET_EV_HUP;
    if (events & EPOLLERR) e |= NET_EV_ERR;
    return e;
}

int PollerCtl(void* poller, int op, int fd, uint32_t events) {
    epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = ToEpollEvents(events);
    ev.data.sock = Fd(fd);
    return epoll_ctl(static_cast<HANDLE>(poller), op, Fd(fd), &ev);
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
    return true;
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
    int err = 0;
    int size = sizeof(err);
    if (getsockopt(Fd(fd), SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&err), &size) != 0)
        return -1;
    if (err == 0) return 0;
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
    return SetSockOptInt(Fd(fd), IPPROTO_IP, IP_TTL, ttl);
}

void* NetPollerCreate() {
    NetPlatformInit();
    return epoll_create1(0);
}

void NetPollerClose(void* poller) {
    if (poller) epoll_close(static_cast<HANDLE>(poller));
}

int NetPollerAdd(void* poller, int fd, uint32_t events) {
    return PollerCtl(poller, EPOLL_CTL_ADD, fd, events) == 0 ? 0 : -1;
}

int NetPollerMod(void* poller, int fd, uint32_t events) {
    return PollerCtl(poller, EPOLL_CTL_MOD, fd, events) == 0 ? 0 : -1;
}

int NetPollerDel(void* poller, int fd) {
    return PollerCtl(poller, EPOLL_CTL_DEL, fd, 0) == 0 ? 0 : -1;
}

int NetPollerWait(void* poller, std::vector<NetPollEvent>& out, int timeoutMs) {
    epoll_event ready[64];
    const int n = epoll_wait(static_cast<HANDLE>(poller), ready, 64, timeoutMs);
    out.clear();
    if (n <= 0) return n < 0 ? -1 : 0;
    for (int i = 0; i < n; ++i) {
        NetPollEvent ev;
        ev.fd = ToFd(ready[i].data.sock);
        ev.events = FromEpollEvents(ready[i].events);
        out.push_back(ev);
    }
    return n;
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
