/*---
    net_platform_posix.cpp — POSIX implementation of net_platform.h
    (net library design §6). Compiled on every non-Windows platform;
    the IO poller set is a VM facility (IoPoller.h / IoPollerPosix.cpp),
    not part of this layer. Blocking conveniences (NetResolve,
    NetIcmpEcho) are worker-thread only. ICMP echo uses
    SOCK_DGRAM/IPPROTO_ICMP (unprivileged ping sockets:
    net.ipv4.ping_group_range on Linux), where the kernel owns the
    ICMP id/sequence and delivers the echo reply to this socket.
---*/
#include "net_platform.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace nlang {
namespace {

int ToAf(NetFamily family) { return family == NetFamily::Inet ? AF_INET : AF_INET6; }

bool SetSockOptInt(int fd, int level, int name, int value) {
    return setsockopt(fd, level, name, &value, sizeof(value)) == 0;
}

void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

int NetConnectResult(int fd) {
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
    return err == 0 ? 0 : -1;
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

bool NetResolve(const std::string& host, int port, std::vector<NetEndpoint>& out) {
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

#ifdef MSG_ERRQUEUE
namespace {
//True when an ICMP error (e.g. destination unreachable) is queued.
//An unconnected DGRAM ICMP socket does not surface ICMP errors through
//recvfrom — with IP_RECVERR they land on the socket error queue, which
//MSG_ERRQUEUE dequeues (external review I5; Linux semantics, hence the
//guard: BSD sockets report those errors through recvfrom's errno).
bool IcmpErrqueuePending(int fd) {
    char ctrl[512];
    msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    //The error queue of this socket type holds only error records, so a
    //successful dequeue means an error arrived; EAGAIN means the queue
    //is empty and the wait really ended in silence.
    return recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) >= 0;
}
}  //namespace
#endif

namespace {
//The echo probes one family per job: prefer IPv4, fall back to IPv6
//(the slice re-arm keeps a dual-stack host reachable either way).
const NetEndpoint* PickEchoEndpoint(const std::vector<NetEndpoint>& eps) {
    for (const NetEndpoint& e : eps)
        if (e.family == NetFamily::Inet) return &e;
    for (const NetEndpoint& e : eps)
        if (e.family == NetFamily::Inet6) return &e;
    return nullptr;
}
}  //namespace

int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len, int timeoutMs) {
    //DGRAM ICMP: the kernel rewrites id/sequence and delivers the echo
    //reply here; SO_RCVTIMEO bounds the blocking recvfrom.
    std::vector<NetEndpoint> eps;
    if (!NetResolve(host, 0, eps)) return -2;
    const NetEndpoint* ep = PickEchoEndpoint(eps);
    if (!ep) return -2;
    const int proto = ep->family == NetFamily::Inet ? IPPROTO_ICMP : IPPROTO_ICMPV6;
    const int fd = socket(ToAf(ep->family), SOCK_DGRAM, proto);
    if (fd < 0) return -2;
    const int one = 1;
#ifdef IP_RECVERR
    if (ep->family == NetFamily::Inet)
        setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
#endif
#ifdef IPV6_RECVERR
    if (ep->family == NetFamily::Inet6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one));
#endif
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = static_cast<long>(timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const auto start = std::chrono::steady_clock::now();
    int rtt = -1;
    if (NetSendTo(fd, payload, len, ep->address, 0) >= 0) {
        uint8_t reply[1536];
        std::string from;
        int port = 0;
        const int n = NetRecvFrom(fd, reply, sizeof(reply), from, port);
        if (n >= 0)
            rtt = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        else {
            //errno after the failure: EAGAIN/EWOULDBLOCK = SO_RCVTIMEO
            //expiry, anything else = hard socket error (I5 verdict map).
            rtt = NetIcmpStatusVerdict(errno);
#ifdef MSG_ERRQUEUE
            if (rtt == -1 && IcmpErrqueuePending(fd))
                rtt = -2;   //the wait ended on a queued ICMP error
#endif
        }
    }
    close(fd);
    return rtt;
}

int NetIcmpStatusVerdict(int err) {
    if (err == 0) return 0;
    if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT) return -1;
    return -2;   //EHOSTUNREACH / ENETUNREACH / ECONNREFUSED / EACCES ...
}

int NetOpenIcmp() {
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) return -1;
    SetNonBlocking(fd);
    return fd;
}

int NetIcmpSend(int fd, const uint8_t* msg, int len, const std::string& addr) {
    return NetSendTo(fd, msg, len, addr, 0);
}

int NetIcmpRecv(int fd, uint8_t* dst, int len, std::string& outAddress) {
    int port = 0;
    return NetRecvFrom(fd, dst, len, outAddress, port);
}

std::string NetLastErrorText() {
    return std::strerror(errno);
}

}  //namespace nlang
