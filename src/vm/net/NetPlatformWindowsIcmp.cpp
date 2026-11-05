/*---
    NetPlatformWindowsIcmp.cpp — ICMP entries of the Windows
    NetPlatform implementation (net library design §6). NetIcmpEcho
    uses the unprivileged IcmpSendEcho2 helper APIs (worker threads
    only); IcmpSocket raw sends use SOCK_RAW/IPPROTO_ICMP, which
    requires administrator privileges (the bind denial carries that
    text through NetLastErrorText, set in the core TU).
---*/
#include "net/NetPlatformInternal.h"

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
    if (h == INVALID_HANDLE_VALUE) return -2;
    std::vector<uint8_t> reply(sizeof(ICMP_ECHO_REPLY) + len + 4);
    auto* rep = reinterpret_cast<ICMP_ECHO_REPLY*>(reply.data());
    const DWORD n = IcmpSendEcho2(h, nullptr, nullptr, nullptr,
                          target.S_un.S_addr,   //IPAddr: the network-order word
                          reinterpret_cast<LPVOID>(const_cast<uint8_t*>(payload)),
                          static_cast<WORD>(len), nullptr, rep,
                          static_cast<DWORD>(reply.size()),
                          static_cast<DWORD>(timeoutMs));
    const int rtt = n > 0 && rep->Status == 0 ? static_cast<int>(rep->RoundTripTime) : -1;
    IcmpCloseHandle(h);
    return rtt;
}

int IcmpEchoV6(const in6_addr& target, const uint8_t* payload, int len, int timeoutMs) {
    HANDLE h = Icmp6CreateFile();
    if (h == INVALID_HANDLE_VALUE) return -2;
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
    const int rtt = n > 0 && rep->Status == 0 ? static_cast<int>(rep->RoundTripTime) : -1;
    IcmpCloseHandle(h);
    return rtt;
}

}  //namespace

int NetIcmpEcho(const std::string& host, const uint8_t* payload, int len, int timeoutMs) {
    NetPlatformInit();
    std::vector<NetEndpoint> eps;
    if (!NetResolve(host, 0, eps)) return -2;
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
    if (s == INVALID_SOCKET) return -1;
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
