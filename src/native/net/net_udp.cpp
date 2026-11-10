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
    for _lastErrorText. UDP send paths park on write interest when the
    send buffer reports full (external review I6); a datagram never
    partial-writes, so a retry replays the whole datagram (no offset
    stash like the TCP write loop needs).
---*/
#include "net_udp.h"

#include "net_options.h"
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

//Numeric-only endpoint resolution shared by the UDP connect/sendTo
//arms (external review Important 2): the guard precedes any socket
//state, and a failed lookup surfaces as -4 with the platform text.
bool ResolveNumeric(NativeHost* h, uint8_t* ret, const std::string& host,
    int port, NetEndpoint* ep) {
    if (!NetIsNumericAddress(host)) { ReturnInt(ret, kNetErrArg); return false; }
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        NetRememberLastError(h);
        ReturnInt(ret, kNetErrPlatform);
        return false;
    }
    *ep = endpoints.front();
    return true;
}

}  //namespace

//--- setup -----------------------------------------------------------------

//udpBind(host, port): open the numeric endpoint's family, bind
//(port 0 = system-assigned, read back through _udpLocalPort). Same
//shape as TcpBind: host names are rejected with kNetErrArg — the
//script layer resolves them on the pool first (external review
//Important 2).
void NetUdpBind(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const int port = ArgInt(a, 1);
    if (!NetIsNumericAddress(host)) { ReturnInt(ret, kNetErrArg); return; }
    NetPlatformInit();   //idempotent through a once flag (R4)
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        NetRememberLastError(h);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    const int fd = NetOpenUdp(ep.family);
    if (fd < 0 || NetBind(fd, ep.address, port) != 0) {
        //Read the OS error before the cleanup close — a successful
        //closesocket resets it to 0 (round-2 M-1; the TcpBind shape).
        std::string text;
        int code = 0;
        NetLastErrorInfo(text, code);
        if (fd >= 0)
            NetClose(fd);
        {
            std::lock_guard<std::mutex> g(st->m_mutex);
            NetSetErrorLocked(st, h, text, code);
        }
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
//Numeric endpoint only (external review Important 2); the guard
//precedes the handle lookup so an argument error is decided before
//any socket state is touched.
void NetUdpConnect(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 1);
    const int port = ArgInt(a, 2);
    NetEndpoint ep;
    if (!ResolveNumeric(h, ret, host, port, &ep)) return;
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    if (NetConnect(fd, ep.address, ep.port) == 0) {
        ReturnInt(ret, 0);
        return;
    }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

//--- data path ---------------------------------------------------------------

//udpSendTo(handle, data, count, host, port, deadlineMs): one sendto,
//parked on write interest and replayed while the send buffer reports
//full (external review I6) — a datagram never partial-writes, so the
//replay is the whole datagram. Numeric endpoint only (external review
//Important 2): the script layer resolves host names on the pool,
//keeping the send loop off getaddrinfo entirely.
void NetUdpSendTo(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView data;
    ArgArray(h, a, 1, &data);
    const int count = ArgInt(a, 2);
    const std::string host = ArgString(h, a, 3);
    const int port = ArgInt(a, 4);
    const long long deadline = ArgLong(a, 5);
    const int self = h->currentContextId(h);
    const std::vector<uint8_t> packed = NetPackBytes(data, count);
    NetEndpoint ep;
    if (!ResolveNumeric(h, ret, host, port, &ep)) return;
    bool claimed = false;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Write, self, &fd, &claimed);
        if (adopt != kNetAdoptOk) {
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        const int n = NetSendTo(fd, packed.data(), count, ep.address, ep.port);
        if (n >= 0) {
            NetRelease(st, handle, NetDir::Write, claimed);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Write, claimed);
            NetRememberLastError(h);
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            NetRelease(st, handle, NetDir::Write, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (NetParkTail(st, handle, NetDir::Write,
                NetClaimAndPark(st, handle, NetDir::Write, self, deadline, h),
                claimed, ret))
            return;
    }
}

//udpSend(handle, data, count, deadlineMs): send() on a connected
//socket; an unconnected one fails with ENOTCONN and surfaces as -4
//with the platform text. Buffer-full sends park on write interest
//and replay the whole datagram (external review I6).
void NetUdpSend(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView data;
    ArgArray(h, a, 1, &data);
    const int count = ArgInt(a, 2);
    const long long deadline = ArgLong(a, 3);
    const int self = h->currentContextId(h);
    const std::vector<uint8_t> packed = NetPackBytes(data, count);
    bool claimed = false;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Write, self, &fd, &claimed);
        if (adopt != kNetAdoptOk) {
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        const int n = NetSend(fd, packed.data(), count);
        if (n >= 0) {
            NetRelease(st, handle, NetDir::Write, claimed);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Write, claimed);
            NetRememberLastError(h);
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            NetRelease(st, handle, NetDir::Write, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (NetParkTail(st, handle, NetDir::Write,
                NetClaimAndPark(st, handle, NetDir::Write, self, deadline, h),
                claimed, ret))
            return;
    }
}

//Commits a received datagram's source: stash the peer on the slot
//(under the same lock as the claim release) for
//_udpLastRecvAddress/Port.
void NetUdpStashPeer(NetExecState* st, int32_t handle, bool claimed,
                     const std::string& from, int fromPort) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    if (NetSocketSlot* slot = st->m_slots.Lookup(handle)) {
        slot->lastPeerAddress = from;
        slot->lastPeerPort = fromPort;
    }
    if (claimed)
        st->m_slots.ReleaseWaiter(handle, NetDir::Read);
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
    //A zero-length buf is an argument error before any socket call:
    //recvfrom(len 0) consumes a queued datagram (returning 0, or
    //WSAEMSGSIZE), and 0 is reserved for a genuine empty datagram
    //(the _tcpRead guard's rationale, 0.12.9 review M-1).
    if (buf.elemCount == 0) { ReturnInt(ret, kNetErrArg); return; }
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
            NetUdpStashPeer(st, handle, claimed, from, fromPort);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Read, claimed);
            NetRememberLastError(h);
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

//udpLastRecvAddress(handle): the source of the last recvFrom (the
//empty string — string has no null value, 0.13.4 — when the slot is
//gone; the script reads it right after a successful receive, so the
//slot is live in practice).
void NetUdpLastRecvAddress(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        ReturnString(h, ret, std::string());
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
    int) { NetApplyFlag(h, ret, a, NetSetBroadcast); }
void NetUdpJoinMulticast(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyText(h, ret, a, NetJoinMulticast); }
void NetUdpLeaveMulticast(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyText(h, ret, a, NetLeaveMulticast); }
void NetUdpSetMulticastTtl(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyInt(h, ret, a, NetSetMulticastTtl); }
void NetUdpSetTtl(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyInt(h, ret, a, NetSetTtl); }
//udpSetSendBufferSize/udpSetRecvBufferSize(handle, bytes): SO_SNDBUF/
//SO_RCVBUF, TCP parity (the kernel may clamp). Also the levers that
//make the send-park path testable.
void NetUdpSetSendBufferSize(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyInt(h, ret, a, NetSetSendBuffer); }
void NetUdpSetRecvBufferSize(NativeHost* h, uint8_t* ret, const uint8_t* a,
    int) { NetApplyInt(h, ret, a, NetSetRecvBuffer); }

//--- state -------------------------------------------------------------------

//udpLocalAddress/udpLocalPort/udpPeerAddress/udpPeerPort: getsockname
//and getpeername arms, identical contracts to the TCP natives (the
//empty string as the failure sentinel for the address text — string
//has no null value, 0.13.4 — R10 code otherwise; an unconnected peer
//query fails getpeername and surfaces as -4).
void NetUdpLocalAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    auto* slot = st->m_slots.Lookup(ArgInt(a, 0));
    if (!slot) {
        st->m_lastErrorText[h->currentContextId(h)] = "socket closed";
        ReturnString(h, ret, std::string());
        return;
    }
    const std::string text = NetLocalAddress(slot->fd);
    if (text.empty()) {
        { std::string errText; int errCode = 0;
          NetLastErrorInfo(errText, errCode);
          NetSetErrorLocked(st, h, errText, errCode); }
        ReturnString(h, ret, std::string());
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
        { std::string errText; int errCode = 0;
          NetLastErrorInfo(errText, errCode);
          NetSetErrorLocked(st, h, errText, errCode); }
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
        st->m_lastErrorText[h->currentContextId(h)] = "socket closed";
        ReturnString(h, ret, std::string());
        return;
    }
    const std::string text = NetPeerAddress(slot->fd);
    if (text.empty()) {
        { std::string errText; int errCode = 0;
          NetLastErrorInfo(errText, errCode);
          NetSetErrorLocked(st, h, errText, errCode); }
        ReturnString(h, ret, std::string());
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
        { std::string errText; int errCode = 0;
          NetLastErrorInfo(errText, errCode);
          NetSetErrorLocked(st, h, errText, errCode); }
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
//The signature is the uniform one every NetRegister*Natives shares; this
//module has no non-blocking native, so reg goes unused.
void NetRegisterUdpNatives(void* registry, [[maybe_unused]] RegisterNativeFn reg,
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
    regBlocking(registry, "net", "_udpSetSendBufferSize", &NetUdpSetSendBufferSize);
    regBlocking(registry, "net", "_udpSetRecvBufferSize", &NetUdpSetRecvBufferSize);
    regBlocking(registry, "net", "_udpLocalAddress", &NetUdpLocalAddress);
    regBlocking(registry, "net", "_udpLocalPort", &NetUdpLocalPort);
    regBlocking(registry, "net", "_udpPeerAddress", &NetUdpPeerAddress);
    regBlocking(registry, "net", "_udpPeerPort", &NetUdpPeerPort);
    regBlocking(registry, "net", "_udpClose", &NetUdpClose);
}

}  //namespace nlang
