/*---
    net_platform_posix.cpp — POSIX implementation of net_platform.h
    (net library design §6), core sockets + resolver. Compiled on every
    non-Windows platform; the IO poller set is a VM facility (IoPoller.h
    / IoPollerPosix.cpp), not part of this layer. Blocking conveniences
    (NetResolve, NetIcmpEcho) are worker-thread only. Shared socket
    helpers live in net_platform_posix_internal.h; the ICMP entries live
    in net_platform_icmp_posix.cpp.
---*/
#include "net_platform_posix_internal.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

namespace nlang {
namespace {

bool SetSockOptInt(int fd, int level, int name, int value) {
    return setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

//Numeric address text (brackets stripped) + port → sockaddr pair.
bool MakeAddr(const std::string& addr, int port, sockaddr_storage& ss, socklen_t& len) {
    std::string text = addr;
    if (!text.empty() && text.front() == '[' && text.back() == ']')
        text = text.substr(1, text.size() - 2);
    std::memset(&ss, 0, sizeof(ss));
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in*>(&ss);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(static_cast<uint16_t>(port));
        sa->sin_addr = v4;
        len = sizeof(*sa);
        return true;
    }
    in6_addr v6;
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in6*>(&ss);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(static_cast<uint16_t>(port));
        sa->sin6_addr = v6;
        len = sizeof(*sa);
        return true;
    }
    return false;
}

//Numeric text of a sockaddr; v4-mapped IPv6 unwraps to dotted IPv4.
std::string AddrText(const sockaddr* sa) {
    char buf[INET6_ADDRSTRLEN] = { 0 };
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr,
                  buf, sizeof(buf));
        return buf;
    }
    if (sa->sa_family == AF_INET6) {
        const in6_addr& a = reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&a)) {
            in_addr v4;
            std::memcpy(&v4, &a.s6_addr[12], 4);
            inet_ntop(AF_INET, &v4, buf, sizeof(buf));
            return buf;
        }
        inet_ntop(AF_INET6, &a, buf, sizeof(buf));
        return buf;
    }
    return std::string();
}

int AddrPort(const sockaddr* sa) {
    return sa->sa_family == AF_INET
        ? ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port)
        : ntohs(reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port);
}

//Query the socket's own bound or peer address.
bool QueryName(int fd, bool peer, sockaddr_storage& ss, socklen_t& len) {
    len = sizeof(ss);
    const int rc = peer ? getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len)
                        : getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len);
    return rc == 0;
}

//Multicast join/leave for both families; interface = the socket's bound
//address (wildcard/any when unbound) so the loop copy arrives (spec §三).
bool MulticastGroupOp(int fd, const std::string& group, bool join) {
    sockaddr_storage ss;
    socklen_t len = 0;
    if (!MakeAddr(group, 0, ss, len)) return false;
    if (ss.ss_family == AF_INET) {
        ip_mreq mreq;
        mreq.imr_multiaddr = reinterpret_cast<sockaddr_in*>(&ss)->sin_addr;
        mreq.imr_interface.s_addr = INADDR_ANY;
        sockaddr_storage bound;
        socklen_t boundLen = 0;
        if (QueryName(fd, false, bound, boundLen)
            && bound.ss_family == AF_INET)
            mreq.imr_interface = reinterpret_cast<sockaddr_in*>(&bound)->sin_addr;
        return setsockopt(fd, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                          &mreq, sizeof(mreq)) == 0;
    }
    ipv6_mreq mreq6;
    mreq6.ipv6mr_multiaddr = reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr;
    //v6 interface index 0 = "any": the bound-address form needs an index
    //lookup; loopback-scoped v6 groups (spec §三) work with 0.
    mreq6.ipv6mr_interface = 0;
    return setsockopt(fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
                      &mreq6, sizeof(mreq6)) == 0;
}

}  //namespace

bool NetPlatformInit() {
    return true;   //no process-level socket subsystem on POSIX
}

int NetOpenTcpStream(NetFamily family) {
    const int fd = socket(ToAf(family), SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    SetNonBlocking(fd);
    return fd;
}

int NetOpenUdp(NetFamily family) {
    const int fd = socket(ToAf(family), SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;
    SetNonBlocking(fd);
    return fd;
}

int NetBind(int fd, const std::string& addr, int port) {
    sockaddr_storage ss;
    socklen_t len = 0;
    if (!MakeAddr(addr, port, ss, len)) return -1;
    if (ss.ss_family == AF_INET6 && AddrText(reinterpret_cast<sockaddr*>(&ss)) == "::")
        SetSockOptInt(fd, IPPROTO_IPV6, IPV6_V6ONLY, 0);   //dual-stack before bind
    return bind(fd, reinterpret_cast<sockaddr*>(&ss), len) == 0 ? 0 : -1;
}

int NetListen(int fd, int backlog) {
    return listen(fd, backlog) == 0 ? 0 : -1;
}

int NetConnect(int fd, const std::string& addr, int port) {
    sockaddr_storage ss;
    socklen_t len = 0;
    if (!MakeAddr(addr, port, ss, len)) return -1;
    if (connect(fd, reinterpret_cast<sockaddr*>(&ss), len) == 0) return 0;
    if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK)
        return kNetInProgress;
    return -1;
}

int NetAccept(int fd) {
    const int c = accept(fd, nullptr, nullptr);
    if (c < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return kNetWouldBlock;
        return -1;
    }
    SetNonBlocking(c);
    return c;
}

bool NetErrWouldBlock(int err) {
    return err == EWOULDBLOCK || err == EAGAIN || err == ENOBUFS;
}

int NetRecv(int fd, uint8_t* dst, int len) {
    const int n = static_cast<int>(recv(fd, dst, static_cast<size_t>(len), 0));
    if (n > 0) return n;
    if (n == 0) return 0;   //EOF
    if (NetErrWouldBlock(errno)) return kNetWouldBlock;
    return -1;
}

int NetSend(int fd, const uint8_t* src, int len) {
    const int n = static_cast<int>(send(fd, src, static_cast<size_t>(len), 0));
    if (n >= 0) return n;
    if (NetErrWouldBlock(errno)) return kNetWouldBlock;
    return -1;
}

int NetShutdownWrite(int fd) {
    return shutdown(fd, SHUT_WR) == 0 ? 0 : -1;
}

int NetConnectResult(int fd, int* soError) {
    if (soError)
        *soError = 0;
    //Zero-timeout writability probe (see net_platform.h): a pending
    //connect also reports SO_ERROR == 0, so a stale park wake must not
    //mint a half-open stream — only a finished connect is writable
    //(a failed one reports POLLOUT|POLLERR, and getsockopt below
    //picks up the reason).
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) <= 0)
        return kNetInProgress;   //not writable yet: still connecting
    int err = 0;
    socklen_t size = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &size) != 0) return -1;
    if (err == 0) return 0;
    //The reason lives in SO_ERROR, not errno: poll and getsockopt both
    //succeeded, so the thread's errno is whatever the connect attempt
    //left behind. Publish the code for the caller and arm the matching
    //text, so NetLastErrorText on the failure path names the real
    //failure (the Windows twin arms the same pair; external review I3).
    if (soError)
        *soError = err;
    g_lastErrorOverride = "connect failed: " + NetErrnoText(err);
    return -1;
}

int NetRecvFrom(int fd, uint8_t* dst, int len,
                std::string& outAddress, int& outPort) {
    sockaddr_storage ss;
    socklen_t ssLen = sizeof(ss);
    const int n = static_cast<int>(recvfrom(fd, dst, static_cast<size_t>(len), 0,
                                            reinterpret_cast<sockaddr*>(&ss), &ssLen));
    if (n < 0) {
        if (NetErrWouldBlock(errno)) return kNetWouldBlock;
        return -1;
    }
    outAddress = AddrText(reinterpret_cast<sockaddr*>(&ss));
    outPort = AddrPort(reinterpret_cast<sockaddr*>(&ss));
    return n;
}

int NetSendTo(int fd, const uint8_t* src, int len,
              const std::string& addr, int port) {
    sockaddr_storage ss;
    socklen_t ssLen = 0;
    if (!MakeAddr(addr, port, ss, ssLen)) return -1;
    const int n = static_cast<int>(sendto(fd, src, static_cast<size_t>(len), 0,
                                          reinterpret_cast<sockaddr*>(&ss), ssLen));
    if (n >= 0) return n;
    if (NetErrWouldBlock(errno)) return kNetWouldBlock;
    return -1;
}

void NetClose(int fd) {
    close(fd);
}

int NetLocalPort(int fd) {
    sockaddr_storage ss;
    socklen_t len = 0;
    return QueryName(fd, false, ss, len) ? AddrPort(reinterpret_cast<sockaddr*>(&ss)) : -1;
}

std::string NetLocalAddress(int fd) {
    sockaddr_storage ss;
    socklen_t len = 0;
    return QueryName(fd, false, ss, len) ? AddrText(reinterpret_cast<sockaddr*>(&ss))
                                         : std::string();
}

int NetPeerPort(int fd) {
    sockaddr_storage ss;
    socklen_t len = 0;
    return QueryName(fd, true, ss, len) ? AddrPort(reinterpret_cast<sockaddr*>(&ss)) : -1;
}

std::string NetPeerAddress(int fd) {
    sockaddr_storage ss;
    socklen_t len = 0;
    return QueryName(fd, true, ss, len) ? AddrText(reinterpret_cast<sockaddr*>(&ss))
                                        : std::string();
}

bool NetSetNoDelay(int fd, bool on) {
    return SetSockOptInt(fd, IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

bool NetSetKeepAlive(int fd, bool on) {
    return SetSockOptInt(fd, SOL_SOCKET, SO_KEEPALIVE, on ? 1 : 0);
}

bool NetSetSendBuffer(int fd, int bytes) {
    return SetSockOptInt(fd, SOL_SOCKET, SO_SNDBUF, bytes);
}

bool NetSetRecvBuffer(int fd, int bytes) {
    return SetSockOptInt(fd, SOL_SOCKET, SO_RCVBUF, bytes);
}

bool NetSetBroadcast(int fd, bool on) {
    return SetSockOptInt(fd, SOL_SOCKET, SO_BROADCAST, on ? 1 : 0);
}

bool NetJoinMulticast(int fd, const std::string& group) {
    return MulticastGroupOp(fd, group, true);
}

bool NetLeaveMulticast(int fd, const std::string& group) {
    return MulticastGroupOp(fd, group, false);
}

bool NetSetMulticastTtl(int fd, int ttl) {
    return SetSockOptInt(fd, IPPROTO_IP, IP_MULTICAST_TTL, ttl);
}

bool NetSetTtl(int fd, int ttl) {
    //the v4 knob is IP_TTL; the v6 equivalent is the unicast hop limit
    sockaddr_storage ss;
    socklen_t len = 0;
    if (QueryName(fd, false, ss, len) && ss.ss_family == AF_INET6)
        return SetSockOptInt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, ttl);
    return SetSockOptInt(fd, IPPROTO_IP, IP_TTL, ttl);
}

int NetSocketPair(int fds[2]) {
    //SOCK_STREAM + NONBLOCK + CLOEXEC: the self-pipe contract
    // (non-blocking, close-on-exec) in one syscall.
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                   0, fds) != 0) {
        fds[0] = fds[1] = -1;
        return -1;
    }
    return 0;
}

bool NetResolve(const std::string& host, int port, std::vector<NetEndpoint>& out,
                int* errOut) {
    out.clear();
    if (errOut)
        *errOut = 0;
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
    const int gaiErr = getaddrinfo(text.c_str(), nullptr, &hints, &list);
    if (gaiErr != 0) {
        if (errOut)
            *errOut = gaiErr;   //POSIX getaddrinfo answers with EAI_* codes
        return false;
    }
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

void NetLastErrorInfo(std::string& outText, int& outCode) {
    if (!g_lastErrorOverride.empty()) {
        //An armed override wins over errno: it was set by the failing
        //operation itself (the connect SO_ERROR verdict, an ICMP
        //denial), while errno here would be a stale survivor of the
        //syscalls between that failure and this reader — the consume-
        //once order of the Windows twin. The numeric code travels on
        //its own channel (the soError out-param), so the override
        //paths report 0 here.
        outText = g_lastErrorOverride;
        g_lastErrorOverride.clear();
        outCode = 0;
        return;
    }
    outCode = errno;
    if (outCode == 0) {
        outText.clear();
        return;
    }
    outText = NetErrnoText(outCode);
}

std::string NetLastErrorText() {
    std::string text;
    int code = 0;
    NetLastErrorInfo(text, code);
    return text;
}

std::string NetTakeLastErrorOverride() {
    //Swap, not copy-and-clear: the ICMP worker's -2 branch consumes the
    //text armed on its own thread — by the socket-creation denial or an
    //echo error verdict — exactly once, as on Windows. An empty return
    //means nothing was armed, and the caller keeps its fallback words.
    std::string text;
    text.swap(g_lastErrorOverride);
    return text;
}

std::string NetResolveErrorText(int resolveErr) {
    switch (resolveErr) {
    case EAI_NONAME:
        return "host not found";
    case EAI_AGAIN:
        return "name resolution temporary failure";
    case EAI_FAIL:
        return "name resolution failed";
    case EAI_NODATA:
        return "no address record for the name";
    default:
        return "name resolution failed";
    }
}

}  //namespace nlang
