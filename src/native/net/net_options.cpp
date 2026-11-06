/*---
    net_options.cpp — the three option-setter arms declared in
    net_options.h, shared by the TCP and UDP option natives.
---*/
#include "net_options.h"

#include "net_slots.h"
#include "net_state.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>

using nlang::native::ArgInt;
using nlang::native::ArgString;
using nlang::native::ReturnInt;

namespace nlang {

void NetApplyFlag(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, bool)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgInt(a, 1) != 0)) { ReturnInt(ret, 0); return; }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

void NetApplyInt(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, int)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgInt(a, 1))) { ReturnInt(ret, 0); return; }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

void NetApplyText(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, const std::string&)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgString(h, a, 1))) { ReturnInt(ret, 0); return; }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

}  //namespace nlang
