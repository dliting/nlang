/*---
    net_platform_icmp_posix.cpp — ICMP entries of the POSIX
    net_platform implementation (net library design §6). NetIcmpEcho
    uses SOCK_DGRAM/IPPROTO_ICMP ping sockets (unprivileged:
    net.ipv4.ping_group_range on Linux), where the kernel owns the
    ICMP id/sequence and delivers the echo reply to this socket; the
    raw IcmpSocket entries reuse the shared datagram send/receive.
    Windows twin: net_platform_icmp_windows.cpp.
---*/
#include "net_platform_posix_internal.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#ifdef MSG_ERRQUEUE
#include <linux/errqueue.h>   //sock_extended_err: the queued error record
#endif
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

namespace nlang {
namespace {

#ifdef MSG_ERRQUEUE
//True when an ICMP error (e.g. destination unreachable) is queued.
//An unconnected DGRAM ICMP socket does not surface ICMP errors through
//recvfrom — with IP_RECVERR they land on the socket error queue, which
//MSG_ERRQUEUE dequeues (external review I5; Linux semantics, hence the
//guard: BSD sockets report those errors through recvfrom's errno).
//On success, queuedErr carries the errno the kernel recorded in the
//error record (sock_extended_err.ee_errno — EHOSTUNREACH for a
//destination unreachable), so the core names the host's answer instead
//of reaching for generic words (external review I4).
bool IcmpErrqueuePending(int fd, int& queuedErr) {
    char ctrl[512];
    msghdr msg;
    std::memset(&msg, 0, sizeof(msg));
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);
    //The error queue of this socket type holds only error records, so a
    //successful dequeue means an error arrived; EAGAIN means the queue
    //is empty and the wait really ended in silence.
    if (recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
        return false;
    queuedErr = 0;
    for (cmsghdr* cm = CMSG_FIRSTHDR(&msg); cm != nullptr;
         cm = CMSG_NXTHDR(&msg, cm)) {
        if ((cm->cmsg_level != IPPROTO_IP && cm->cmsg_level != IPPROTO_IPV6) ||
            cm->cmsg_len < CMSG_LEN(sizeof(sock_extended_err)))
            continue;
        const auto* ee =
            reinterpret_cast<const sock_extended_err*>(CMSG_DATA(cm));
        if (ee->ee_errno != 0)
            queuedErr = ee->ee_errno;
        break;   //the first RECVERR record describes this failure
    }
    return true;
}
#endif

//The echo probes one family per job: prefer IPv4, fall back to IPv6
//(the slice re-arm keeps a dual-stack host reachable either way).
const NetEndpoint* PickEchoEndpoint(const std::vector<NetEndpoint>& eps) {
    for (const NetEndpoint& e : eps)
        if (e.family == NetFamily::Inet) return &e;
    for (const NetEndpoint& e : eps)
        if (e.family == NetFamily::Inet6) return &e;
    return nullptr;
}

//Smallest receive wait this layer accepts. SO_RCVTIMEO with a {0, 0}
//timeval means "block forever" on Linux (unlike Windows' IcmpSendEcho2,
//where 0 means "no wait"), and the caller's slice-capped timeout
//legitimately reaches 0 when the deadline falls inside the current
//millisecond — a black-hole host could then park this pool worker and
//its ICMP socket past every deadline (external review I6).
constexpr int kMinEchoWaitMs = 1;

//A fresh echo socket of the endpoint's family: the reply wait bounded
//by SO_RCVTIMEO, and where the platform supports it IP_RECVERR so a
//queued ICMP error ends the wait instead of a silent timeout.
//Returns the fd, or -1 when the socket did not come up (errno says why).
int IcmpEchoSocket(NetFamily family, int timeoutMs) {
    //Cast: the two IPPROTO_* constants come from different enum types
    //(netinet/ip.h and netinet/in.h), which a conditional mixing the
    //two enumerators would compare across enums.
    const int proto = family == NetFamily::Inet
        ? static_cast<int>(IPPROTO_ICMP) : static_cast<int>(IPPROTO_ICMPV6);
    const int fd = socket(ToAf(family), SOCK_DGRAM, proto);
    if (fd < 0) {
        //Arm the denial text — EACCES under the stock Linux
        //net.ipv4.ping_group_range gate the icmp manual warns about:
        //the core would otherwise call a host that resolved fine "host
        //has no usable address" (external review I4).
        g_lastErrorOverride =
            "ICMP echo socket creation failed: " + NetErrnoText(errno);
        return -1;
    }
    const int one = 1;
#ifdef IP_RECVERR
    if (family == NetFamily::Inet)
        setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
#endif
#ifdef IPV6_RECVERR
    if (family == NetFamily::Inet6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one));
#endif
    //Floor the wait at kMinEchoWaitMs: timeoutMs reaches 0 in the last
    //millisecond before the deadline, and {0, 0} there would wait
    //forever rather than not at all.
    const int waitMs = timeoutMs < kMinEchoWaitMs ? kMinEchoWaitMs : timeoutMs;
    timeval tv;
    tv.tv_sec = waitMs / 1000;
    tv.tv_usec = static_cast<long>(waitMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

}  //namespace

int NetIcmpStatusVerdict(int err) {
    if (err == 0) return 0;
    if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT) return -1;
    return -2;   //EHOSTUNREACH / ENETUNREACH / ECONNREFUSED / EACCES ...
}

int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len,
                int timeoutMs, int* resolveErrOut) {
    //DGRAM ICMP: the kernel rewrites id/sequence and delivers the echo
    //reply here; SO_RCVTIMEO bounds the blocking recvfrom.
    if (resolveErrOut)
        *resolveErrOut = 0;
    std::vector<NetEndpoint> eps;
    if (!NetResolve(host, 0, eps, resolveErrOut)) return -2;
    const NetEndpoint* ep = PickEchoEndpoint(eps);
    if (!ep) return -2;
    const int fd = IcmpEchoSocket(ep->family, timeoutMs);
    if (fd < 0) return -2;
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
            //expiry, anything else = hard socket error (I5 verdict
            //map). Capture it before the verdict: the errqueue dequeue
            //below runs its own syscalls and would overwrite errno.
            const int recvErr = errno;
            rtt = NetIcmpStatusVerdict(recvErr);
            if (rtt == -2)
                g_lastErrorOverride = NetErrnoText(recvErr);
#ifdef MSG_ERRQUEUE
            if (rtt == -1) {
                int queuedErr = 0;
                if (IcmpErrqueuePending(fd, queuedErr)) {
                    rtt = -2;   //the wait ended on a queued ICMP error
                    g_lastErrorOverride = queuedErr != 0
                        ? NetErrnoText(queuedErr)
                        : std::string("host unreachable (ICMP error received)");
                }
            }
#endif
        }
    }
    close(fd);
    return rtt;
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

}  //namespace nlang
