/*---
    net_udp.cpp — the UDP datagram natives (net library design §7):
    bind/sendTo/connect/send, the two receive arms, the option setters
    and the state queries. The receive arms park on read interest in
    the _tcpRead shape (claim+park helpers: net_tcp_park.h); _udpRecv
    registers the very same native as _tcpRead — a datagram arriving
    on a UDP socket is readable data, and the loop never inspects the
    slot kind. The last recvFrom source is stashed on the slot
    (lastPeerAddress/lastPeerPort) so the script wrapper can mint its
    out params after the receive completed (R2).

    Error contract is the TCP one (R10): closed slot -1, timeout -2,
    concurrent claim -3, platform failure -4 with the OS text recorded
    for _lastErrorText. UDP send paths never park: a non-blocking
    datagram send either hands the buffer over or fails.
---*/
#include "net_udp.h"

#include "net_platform.h"
#include "net_slots.h"
#include "net_state.h"
#include "net_tcp.h"
#include "net_tcp_park.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::ArgString;
using nlang::native::ReturnInt;
using nlang::native::ReturnString;

namespace nlang {
namespace {

//Shared option-setter shapes (same discipline as the TCP option
//natives): resolve the fd under the mutex, apply one socket call,
//map a closed slot to -1 and a failure to -4 with the platform text
//recorded. The bool travels as an int32 cell (the VM's bool
//representation).
void ApplyFlag(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, bool)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgInt(a, 1) != 0)) { ReturnInt(ret, 0); return; }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

//Int-valued variant of ApplyFlag (TTL setters).
void ApplyInt(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, int)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgInt(a, 1))) { ReturnInt(ret, 0); return; }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

//Text-valued variant of ApplyFlag (multicast group membership).
void ApplyText(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, const std::string&)) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (apply(fd, ArgString(h, a, 1))) { ReturnInt(ret, 0); return; }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

}  //namespace

//--- setup -----------------------------------------------------------------

//udpBind(host, port): resolve, open the first endpoint's family, bind
//(port 0 = system-assigned, read back through _udpLocalPort). Same
//shape as TcpBind: the resolve runs outside the mutex.
void NetUdpBind(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const int port = ArgInt(a, 1);
    NetPlatformInit();   //idempotent through a once flag (R4)
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    const int fd = NetOpenUdp(ep.family);
    if (fd < 0 || NetBind(fd, ep.address, port) != 0) {
        if (fd >= 0)
            NetClose(fd);
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    int32_t handle = 0;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        handle = st->m_slots.Mint(fd, kNetKindUdp);   }
    ReturnInt(ret, handle);
}

//udpConnect(host, port): the immediate associate(2) — UDP connect
//never has an in-progress state (contrast the TCP verdict loop).
void NetUdpConnect(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 1);
    const int port = ArgInt(a, 2);
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (NetConnect(fd, ep.address, ep.port) == 0) {
        ReturnInt(ret, 0);
        return;
    }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

//--- data path ---------------------------------------------------------------

//udpSendTo(handle, data, count, host, port): resolve then one
//sendto — datagrams never partial-write, so there is nothing to park
//for (v1 resolves per call; the numeric fast path keeps the common
//loop off getaddrinfo).
void NetUdpSendTo(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    NativeArrayView data;
    ArgArray(h, a, 1, &data);
    const int count = ArgInt(a, 2);
    const std::string host = ArgString(h, a, 3);
    const int port = ArgInt(a, 4);
    const std::vector<uint8_t> packed = NetPackBytes(data, count);
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    const int n = NetSendTo(fd, packed.data(), count, ep.address, ep.port);
    if (n >= 0) { ReturnInt(ret, n); return; }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

//udpSend(handle, data, count): send() on a connected socket; an
//unconnected one fails with ENOTCONN and surfaces as -4 with the
//platform text.
void NetUdpSend(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    NativeArrayView data;
    ArgArray(h, a, 1, &data);
    const int count = ArgInt(a, 2);
    const std::vector<uint8_t> packed = NetPackBytes(data, count);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    const int n = NetSend(fd, packed.data(), count);
    if (n >= 0) { ReturnInt(ret, n); return; }
    NetRememberLastError(st);
    ReturnInt(ret, kNetErrPlatform);
}

//udpRecvFrom(handle, buf, deadlineMs): the _tcpRead park loop with
//recvfrom; on success the source is stashed on the slot (under the
//same lock as the claim release) for _udpLastRecvAddress/Port. n == 0
//is an empty datagram here, not EOF (TCP's meaning).
void NetUdpRecvFrom(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView buf;
    ArgArray(h, a, 1, &buf);
    const long long deadline = ArgLong(a, 2);
    const int self = h->currentContextId(h);   //R14
    std::vector<uint8_t> scratch(static_cast<size_t>(buf.elemCount));
    bool claimed = false;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Read, self, &fd, &claimed);
        if (adopt != kNetAdoptOk) {
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        std::string from;
        int fromPort = 0;
        const int n = NetRecvFrom(fd, scratch.data(),
            static_cast<int>(buf.elemCount), from, fromPort);
        if (n >= 0) {   //a datagram (possibly empty)
            NetUnpackBytes(buf, scratch.data(), n);
            std::lock_guard<std::mutex> g(st->m_mutex);
            if (NetSocketSlot* slot = st->m_slots.Lookup(handle)) {
                slot->lastPeerAddress = from;
                slot->lastPeerPort = fromPort;
            }
            if (claimed)
                st->m_slots.ReleaseWaiter(handle, NetDir::Read);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Read, claimed);
            NetRememberLastError(st);
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            NetRelease(st, handle, NetDir::Read, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (NetParkTail(st, handle, NetDir::Read,
                NetClaimAndPark(st, handle, NetDir::Read, self, deadline, h),
                claimed, ret))
            return;
    }
}

//udpLastRecvAddress(handle): the source of the last recvFrom (null
//sentinel 0 when the slot is gone — the script reads it right after a
//successful receive, so the slot is live in practice).
void NetUdpLastRecvAddress(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        ReturnInt(ret, 0);
        return;
    }
    ReturnString(h, ret, slot->lastPeerAddress);
}

//udpLastRecvPort(handle): the source port of the last recvFrom.
void NetUdpLastRecvPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    ReturnInt(ret, slot ? slot->lastPeerPort : kNetErrClosed);
}

//--- options -----------------------------------------------------------------

void NetUdpSetBroadcast(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { ApplyFlag(h, ret, a, NetSetBroadcast); }
void NetUdpJoinMulticast(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { ApplyText(h, ret, a, NetJoinMulticast); }
void NetUdpLeaveMulticast(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { ApplyText(h, ret, a, NetLeaveMulticast); }
void NetUdpSetMulticastTtl(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { ApplyInt(h, ret, a, NetSetMulticastTtl); }
void NetUdpSetTtl(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { ApplyInt(h, ret, a, NetSetTtl); }

//--- state -------------------------------------------------------------------

//udpLocalAddress/udpLocalPort/udpPeerAddress/udpPeerPort: getsockname
//and getpeername arms, identical contracts to the TCP natives (null
//sentinel for the address text, R10 code otherwise; an unconnected
//peer query fails getpeername and surfaces as -4).
void NetUdpLocalAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        st->m_lastErrorText = "socket closed";
        ReturnInt(ret, 0);
        return;
    }
    const std::string text = NetLocalAddress(slot->fd);
    if (text.empty()) {
        st->m_lastErrorText = NetLastErrorText();
        ReturnInt(ret, 0);
        return;
    }
    ReturnString(h, ret, text);
}

void NetUdpLocalPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        ReturnInt(ret, kNetErrClosed);
        return;
    }
    const int port = NetLocalPort(slot->fd);
    if (port < 0) {
        st->m_lastErrorText = NetLastErrorText();
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    ReturnInt(ret, port);
}

void NetUdpPeerAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        st->m_lastErrorText = "socket closed";
        ReturnInt(ret, 0);
        return;
    }
    const std::string text = NetPeerAddress(slot->fd);
    if (text.empty()) {
        st->m_lastErrorText = NetLastErrorText();
        ReturnInt(ret, 0);
        return;
    }
    ReturnString(h, ret, text);
}

void NetUdpPeerPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        ReturnInt(ret, kNetErrClosed);
        return;
    }
    const int port = NetPeerPort(slot->fd);
    if (port < 0) {
        st->m_lastErrorText = NetLastErrorText();
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    ReturnInt(ret, port);
}

//udpClose(handle): idempotent, the TcpClose contract (MarkClosed
//under the mutex, wakeIoWaiters after the unlock — the close-side
//wake of the lost-wakeup window closure).
void NetUdpClose(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(handle);
        st->m_slots.MarkClosed(handle);   }
    if (fd >= 0)
        h->wakeIoWaiters(h, fd);
    ReturnInt(ret, 0);
}

//--- registration ------------------------------------------------------------

//Declared in net_udp.h: net_native.cpp's module init calls this.
//_udpRecv registers NetTcpRead — the recv park loop is socket-kind
//agnostic (a datagram is readable data; n == 0 reads as an empty
//datagram to the UDP script face, as EOF to TCP's).
void NetRegisterUdpNatives(void* registry, RegisterNativeFn reg,
    RegisterNativeBlockingFn regBlocking) {
    regBlocking(registry, "net", "_udpBind", &NetUdpBind);
    regBlocking(registry, "net", "_udpSendTo", &NetUdpSendTo);
    regBlocking(registry, "net", "_udpConnect", &NetUdpConnect);
    regBlocking(registry, "net", "_udpSend", &NetUdpSend);
    regBlocking(registry, "net", "_udpRecv", &NetTcpRead);
    regBlocking(registry, "net", "_udpRecvFrom", &NetUdpRecvFrom);
    regBlocking(registry, "net", "_udpLastRecvAddress", &NetUdpLastRecvAddress);
    regBlocking(registry, "net", "_udpLastRecvPort", &NetUdpLastRecvPort);
    regBlocking(registry, "net", "_udpSetBroadcast", &NetUdpSetBroadcast);
    regBlocking(registry, "net", "_udpJoinMulticast", &NetUdpJoinMulticast);
    regBlocking(registry, "net", "_udpLeaveMulticast", &NetUdpLeaveMulticast);
    regBlocking(registry, "net", "_udpSetMulticastTtl", &NetUdpSetMulticastTtl);
    regBlocking(registry, "net", "_udpSetTtl", &NetUdpSetTtl);
    regBlocking(registry, "net", "_udpLocalAddress", &NetUdpLocalAddress);
    regBlocking(registry, "net", "_udpLocalPort", &NetUdpLocalPort);
    regBlocking(registry, "net", "_udpPeerAddress", &NetUdpPeerAddress);
    regBlocking(registry, "net", "_udpPeerPort", &NetUdpPeerPort);
    regBlocking(registry, "net", "_udpClose", &NetUdpClose);
}

}  //namespace nlang
