/*---
    net_platform_posix_internal.h — socket helpers shared by the two
    POSIX net_platform translation units (core sockets and ICMP). Not
    part of the package surface; the Windows twin is
    net_platform_internal.h.
---*/
#pragma once

#include "net_platform.h"

#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <system_error>

namespace nlang {

inline int ToAf(NetFamily family) {
    return family == NetFamily::Inet ? AF_INET : AF_INET6;
}

inline void SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//Human-readable phrase for an errno value: the platform message plus a
//stable numeric anchor, the shape NetLastErrorInfo builds from errno.
//Factored out so the arming sites — the connect SO_ERROR verdict, the
//ICMP socket denial and the echo error paths — can name the real
//failure instead of leaving the reader to a stale errno (external
//review I3/I4).
inline std::string NetErrnoText(int err) {
    //std::strerror hands back one process-wide buffer, so two threads
    //naming an error at the same moment (natives run on VM threads and
    //pool workers) can overwrite each other's text; error_code::message()
    //builds its own string from the same platform message table.
    std::string text = std::error_code(err, std::generic_category()).message();
    //Locale-independent anchor for logs: the OS text above follows the
    //system language, the number is stable (I-5, the Windows shape).
    text += " (error " + std::to_string(err) + ")";
    return text;
}

//Error text armed on the calling thread by the failing operation itself,
//consumed exactly once by NetLastErrorInfo / NetTakeLastErrorOverride —
//the Windows twin's channel (net_platform_internal.h).
inline thread_local std::string g_lastErrorOverride;

}  //namespace nlang
