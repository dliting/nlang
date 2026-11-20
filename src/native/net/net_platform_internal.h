/*---
    net_platform_internal.h — Winsock helpers shared by the two Windows
    net_platform translation units (core sockets and ICMP). Not part of
    the package surface; include after winsock2.h is available.
---*/
#pragma once

#include "net_platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <string>

namespace nlang {

//Windows socket handles are 32-bit kernel handles even in 64-bit
//processes, so the SOCKET <-> int round trip is lossless.
inline SOCKET Fd(int fd) {
    return static_cast<SOCKET>(static_cast<uintptr_t>(static_cast<intptr_t>(fd)));
}
inline int ToFd(SOCKET s) { return static_cast<int>(static_cast<uintptr_t>(s)); }

inline int ToAf(NetFamily family) {
    return family == NetFamily::Inet ? AF_INET : AF_INET6;
}

inline bool SetSockOptInt(SOCKET s, int level, int name, int value) {
    return setsockopt(s, level, name,
                      reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

inline void SetNonBlocking(SOCKET s) {
    u_long one = 1;
    ioctlsocket(s, FIONBIO, &one);
}

//Numeric address text (brackets stripped) + port → sockaddr pair.
inline bool MakeAddr(const std::string& addr, int port,
                     sockaddr_storage& ss, int& len) {
    std::string text = addr;
    if (!text.empty() && text.front() == '[' && text.back() == ']')
        text = text.substr(1, text.size() - 2);
    std::memset(&ss, 0, sizeof(ss));
    in_addr v4;
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in*>(&ss);
        sa->sin_family = AF_INET;
        sa->sin_port = htons(static_cast<u_short>(port));
        sa->sin_addr = v4;
        len = sizeof(*sa);
        return true;
    }
    in6_addr v6;
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        auto* sa = reinterpret_cast<sockaddr_in6*>(&ss);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons(static_cast<u_short>(port));
        sa->sin6_addr = v6;
        len = sizeof(*sa);
        return true;
    }
    return false;
}

//Numeric text of a sockaddr; v4-mapped IPv6 unwraps to dotted IPv4.
inline std::string AddrText(const sockaddr* sa) {
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

inline int AddrPort(const sockaddr* sa) {
    return sa->sa_family == AF_INET
        ? ntohs(reinterpret_cast<const sockaddr_in*>(sa)->sin_port)
        : ntohs(reinterpret_cast<const sockaddr_in6*>(sa)->sin6_port);
}

//Query the socket's own bound or peer address.
inline bool QueryName(int fd, bool peer, sockaddr_storage& ss, int& len) {
    len = sizeof(ss);
    const int rc = peer ? getpeername(Fd(fd), reinterpret_cast<sockaddr*>(&ss), &len)
                        : getsockname(Fd(fd), reinterpret_cast<sockaddr*>(&ss), &len);
    return rc == 0;
}

//Human-readable override for NetLastErrorText (raw-ICMP admin denial,
//SO_ERROR verdicts); handed to the core TU by NetTakeLastErrorOverride
//(net_platform.h), which consumes and clears it.
inline thread_local std::string g_lastErrorOverride;

}  //namespace nlang
