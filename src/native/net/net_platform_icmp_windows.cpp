/*---
    net_platform_icmp_windows.cpp — ICMP entries of the Windows
    net_platform implementation (net library design §6). NetIcmpEcho
    uses the unprivileged IcmpSendEcho2 helper APIs (worker threads
    only); IcmpSocket raw sends use SOCK_RAW/IPPROTO_ICMP, which
    requires administrator privileges (the bind denial carries that
    text through NetLastErrorText, set in the core TU).
---*/
#include "net_platform_internal.h"

#include <windows.h>
#include <iphlpapi.h>   //IPAddr / iptypes.h, consumed by ipexport.h
#include <ipexport.h>   //ICMP_ECHO_REPLY / ICMPV6_ECHO_REPLY
#include <icmpapi.h>    //IcmpCreateFile / IcmpSendEcho2

#include <vector>

namespace nlang {
namespace {

//One IcmpSendEcho2/Icmp6SendEcho2 round trip on a fresh handle.
int IcmpEchoV4(const in_addr& target, const uint8_t* payload, int len, int timeoutMs) {
    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) {
        //Resource exhaustion, not an address problem — the override
        //carries the honest phrase to the worker's splice (round-2 M-3).
        g_lastErrorOverride = "ICMP echo handle creation failed";
        return -2;
    }
    std::vector<uint8_t> reply(sizeof(ICMP_ECHO_REPLY) + len + 4);
    auto* rep = reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data());
    const DWORD n = IcmpSendEcho2(h, nullptr, nullptr, nullptr,
                          target.S_un.S_addr,   //IPAddr: the network-order word
                          reinterpret_cast<LPVOID>(const_cast<uint8_t*>(payload)),
                          static_cast<WORD>(len), nullptr, rep,
                          static_cast<DWORD>(reply.size()),
                          static_cast<DWORD>(timeoutMs));
    //Status carries the verdict: 0 = reply, IP_REQ_TIMED_OUT = no
    //answer, everything else = unreachable/error (external review I5).
    //The unreachable family arms the override so the worker's splice
    //names it honestly (round-3 wording nit).
    const int rtt = n > 0 ? NetIcmpStatusVerdict(static_cast<int>(rep->Status)) : -1;
    if (rtt == -2)
        g_lastErrorOverride = "host unreachable";
    IcmpCloseHandle(h);
    return rtt;
}

int IcmpEchoV6(const in6_addr& target, const uint8_t* payload, int len, int timeoutMs) {
    HANDLE h = Icmp6CreateFile();
    if (h == INVALID_HANDLE_VALUE) {
        g_lastErrorOverride = "ICMP echo handle creation failed";
        return -2;
    }
    std::vector<uint8_t> reply(sizeof(ICMPV6_ECHO_REPLY) + len + 4);
    auto* rep = reinterpret_cast<ICMPV6_ECHO_REPLY*>(reply.data());
    sockaddr_in6 src = { };
    src.sin6_family = AF_INET6;   //source "::" (any)
    sockaddr_in6 dst = { };
    dst.sin6_family = AF_INET6;
    dst.sin6_addr = target;
    const DWORD n = Icmp6SendEcho2(h, nullptr, nullptr, nullptr,
                          &src, &dst, reinterpret_cast<LPVOID>(const_cast<uint8_t*>(payload)),
                          static_cast<WORD>(len), nullptr, rep,
                          static_cast<DWORD>(reply.size()),
                          static_cast<DWORD>(timeoutMs));
    const int rtt = n > 0 ? NetIcmpStatusVerdict(static_cast<int>(rep->Status)) : -1;
    if (rtt == -2)
        g_lastErrorOverride = "host unreachable";   //honest splice (v4 twin)
    IcmpCloseHandle(h);
    return rtt;
}

}  //namespace

int NetIcmpStatusVerdict(int status) {
    if (status == IP_SUCCESS) return 0;
    if (status == IP_REQ_TIMED_OUT) return -1;   //no answer before the timeout
    return -2;   //IP_DEST_*_UNREACHABLE / IP_TTL_EXPIRED_* / IP_GENERAL_FAILURE
}

int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len,
                int timeoutMs, int* resolveErrOut) {
    NetPlatformInit();
    if (resolveErrOut) *resolveErrOut = 0;
    std::vector<NetEndpoint> eps;
    if (!NetResolve(host, 0, eps, resolveErrOut)) return -2;
    for (const NetEndpoint& ep : eps)
        if (ep.family == NetFamily::Inet) {
            in_addr a;
            inet_pton(AF_INET, ep.address.c_str(), &a);
            return IcmpEchoV4(a, payload, len, timeoutMs);
        }
    for (const NetEndpoint& ep : eps)
        if (ep.family == NetFamily::Inet6) {
            in6_addr a;
            inet_pton(AF_INET6, ep.address.c_str(), &a);
            return IcmpEchoV6(a, payload, len, timeoutMs);
        }
    return -2;
}

int NetOpenIcmp() {
    NetPlatformInit();
    const SOCKET s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s == INVALID_SOCKET) {
        //The non-admin failure is at socket creation, not at bind —
        //surface the promised text here too (M-3; NetBind's WSAEACCES
        //path covers the bind-stage denial).
        if (WSAGetLastError() == WSAEACCES)
            g_lastErrorOverride =
                "raw ICMP requires administrator privileges on Windows";
        return -1;
    }
    SetNonBlocking(s);
    return ToFd(s);
}

int NetIcmpSend(int fd, const uint8_t* msg, int len, const std::string& addr) {
    return NetSendTo(fd, msg, len, addr, 0);
}

int NetIcmpRecv(int fd, uint8_t* dst, int len, std::string& outAddress) {
    int port = 0;
    return NetRecvFrom(fd, dst, len, outAddress, port);
}

}  //namespace nlang
