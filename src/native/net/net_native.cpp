/*---
    net_native.cpp — the nlang_net module entry (net library design §6):
    the internal primitives the net.n script surface calls (underscore
    names are module-private by convention; the public face is script).
    The socket natives register on the blocking channel — the r1 model releases the
    GIL around blocking natives, so the slot table and the last-error
    text are guarded by NetExecState::m_mutex (plan ruling R9) and the
    lock is scoped to the state accesses only (a slow getaddrinfo in
    bind must not serialize other carriers' calls). The two pure byte
    converters (_toBytes/_fromBytes) never wait, so they register on
    the regular channel. Failures return the
    R10 error codes; -4 paths record the platform text for
    _lastErrorText. Exception mapping lives in the script (_throwNet).
---*/
#include "net_icmp.h"
#include "net_jobs.h"
#include "net_platform.h"
#include "net_state.h"
#include "net_tcp.h"
#include "net_udp.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::ArgString;
using nlang::native::NewArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;
using nlang::native::ReturnLong;
using nlang::native::ReturnString;

using nlang::kNetErrClosed;
using nlang::kNetErrPlatform;
using nlang::kNetKindTcpListener;
using nlang::NetBind;
using nlang::NetClose;
using nlang::NetEndpoint;
using nlang::NetExecState;
using nlang::NetLastErrorText;
using nlang::NetListen;
using nlang::NetLocalAddress;
using nlang::NetLocalPort;
using nlang::NetOpenTcpStream;
using nlang::NetPlatformInit;
using nlang::NetResolve;
using nlang::NetState;
using nlang::NetTcpAccept;
using nlang::NetTcpConnectTry;
using nlang::NetTcpIsClosed;
using nlang::NetTcpPeerAddress;
using nlang::NetTcpPeerPort;
using nlang::NetTcpRead;
using nlang::NetTcpResolveEndpoint;
using nlang::NetTcpResolveRelease;
using nlang::NetTcpResolveStart;
using nlang::NetTcpResolveWait;
using nlang::NetTcpSetKeepAlive;
using nlang::NetTcpSetNoDelay;
using nlang::NetTcpSetRecvBufferSize;
using nlang::NetTcpSetSendBufferSize;
using nlang::NetTcpShutdownWrite;
using nlang::NetTcpWrite;

namespace {

//TcpListener accept-queue depth (the backlog every listener API hides
//behind a constant). Sized for burst accepts: one listener must absorb
//thousands of simultaneous connects while the acceptor drains at its
//own pace (Node listens with 511, Go with the system maximum; Windows
//clamps the effective depth to its own cap). 64 overflowed the 10k
//benchmark (tools/net_bench/net_bench_10k.n): dropped SYNs pushed
//clients past their connect timeout.
constexpr int kNetListenBacklog = 512;

//Records the failure text for _lastErrorText (R10: the -4 code carries
//its message through the state, not through raiseException). Takes the
//state mutex — call only while it is NOT already held.
void SetNetError(NetExecState* st, const std::string& text) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_lastErrorText = text;
}

// nowMs: the host's monotonic millisecond clock — the same clock
// domain as the park deadlines (R12: the script converts relative
// timeouts to absolute deadlines with it). Pure clock read, but on
// the blocking channel like every other net native (no channel mix).
void NowMs(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    ReturnLong(ret, h->nowMs(h));
}

// lastErrorText: the text recorded by the most recent -4 failure.
void LastErrorText(NativeHost* h, uint8_t* ret, const uint8_t*, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    ReturnString(h, ret, st->m_lastErrorText);
}

// toBytes/fromBytes: the module-internal UTF-8 converters (r1 note 5:
// builtin packages never import each other, so net.n's convenience face
// carries its own copy; the public face stays io.toBytes/fromBytes).
// Same narrow-cell convention as the io natives (task 5 semantics):
// byte cells are int32 carrying the value in the low byte.
void NetToBytes(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    const std::string text = ArgString(h, a, 0);
    const int32_t len = static_cast<int32_t>(text.size());
    const int32_t handle = NewArray(h, NEK_Byte, len);
    NativeArrayView view;
    h->getArrayView(h, handle, &view);
    int32_t* cells = static_cast<int32_t*>(view.data);
    for (int32_t i = 0; i < len; ++i)
        cells[i] = static_cast<unsigned char>(
            text[static_cast<size_t>(i)]);
    ReturnInt(ret, handle);
}

void NetFromBytes(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NativeArrayView view;
    ArgArray(h, a, 0, &view);
    if (view.rank != 1
        || (view.elemKind != NEK_Byte && view.elemKind != NEK_UByte)) {
        Raise(h, NEXC_IOException,
            "net._fromBytes: expected a rank-1 byte array");
        return;
    }
    const int32_t* cells = static_cast<const int32_t*>(view.data);
    const int64_t o = view.offset;
    const int64_t step = view.strides[0];
    std::string content;
    content.reserve(static_cast<size_t>(view.elemCount));
    for (int64_t i = 0; i < view.elemCount; ++i)
        content.push_back(static_cast<char>(static_cast<unsigned char>(
            cells[o + i * step])));
    ReturnString(h, ret, content);
}

// tcpBind: resolve, open, bind, listen; the first resolved endpoint
// picks the family (host "::" opens a dual-stack listener: NetBind
// clears IPV6_V6ONLY for it); port 0 is system-assigned and read back
// through _tcpLocalPort. Migrated from the VM intrinsic (task 6): the
// Raise became the R10 code + last-error text; the script mints the
// TcpListener object around the returned slot handle.
void TcpBind(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const int port = ArgInt(a, 1);
    //Idempotent through a once flag (ruling R4); every net native
    //needs the socket subsystem up (Winsock on Windows).
    NetPlatformInit();
    const std::string context = "bind " + host + ":" + std::to_string(port);
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, port, endpoints) || endpoints.empty()) {
        SetNetError(st, context + " failed: " + NetLastErrorText());
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    const int fd = NetOpenTcpStream(ep.family);
    if (fd < 0 || NetBind(fd, ep.address, port) != 0
        || NetListen(fd, kNetListenBacklog) != 0) {
        if (fd >= 0)
            NetClose(fd);
        SetNetError(st, context + " failed: " + NetLastErrorText());
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    int32_t handle = 0;
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        handle = st->m_slots.Mint(fd, kNetKindTcpListener);
    }
    ReturnInt(ret, handle);
}

// tcpLocalAddress: the bound address text; null (handle 0 — the null
// literal is the sentinel 0) on a closed slot or a getsockname
// failure, which the script maps through _throwNet.
void TcpLocalAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
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

// tcpLocalPort: the system-assigned port (port-0 bind); negative R10
// code on failure.
void TcpLocalPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
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

// tcpClose: idempotent. MarkClosed runs under the mutex (deferred
// while waiters remain, ruling R5); wakeIoWaiters runs after the
// unlock — the close-side wake of the lost-wakeup window closure
// (plan task 7 discipline, applied to the skeleton's close as well).
void TcpClose(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    int fd = -1;
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(handle);
        st->m_slots.MarkClosed(handle);
    }
    if (fd >= 0)
        h->wakeIoWaiters(h, fd);
    ReturnInt(ret, 0);
}

}  //namespace

// Module entry point. Every net native registers on the blocking
// channel: the r1 contract releases the GIL around the call, which is
// what makes the slot-table mutex (R9) both necessary and sufficient.
NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "net", "_nowMs", &NowMs);
    regBlocking(registry, "net", "_lastErrorText", &LastErrorText);
    regBlocking(registry, "net", "_tcpBind", &TcpBind);
    regBlocking(registry, "net", "_tcpLocalAddress", &TcpLocalAddress);
    regBlocking(registry, "net", "_tcpLocalPort", &TcpLocalPort);
    regBlocking(registry, "net", "_tcpClose", &TcpClose);
    regBlocking(registry, "net", "_tcpAccept", &NetTcpAccept);
    regBlocking(registry, "net", "_tcpResolveStart", &NetTcpResolveStart);
    regBlocking(registry, "net", "_tcpResolveWait", &NetTcpResolveWait);
    regBlocking(registry, "net", "_tcpResolveEndpoint", &NetTcpResolveEndpoint);
    regBlocking(registry, "net", "_tcpResolveRelease", &NetTcpResolveRelease);
    regBlocking(registry, "net", "_tcpConnectTry", &NetTcpConnectTry);
    regBlocking(registry, "net", "_tcpRead", &NetTcpRead);
    regBlocking(registry, "net", "_tcpWrite", &NetTcpWrite);
    regBlocking(registry, "net", "_tcpShutdownWrite", &NetTcpShutdownWrite);
    regBlocking(registry, "net", "_tcpIsClosed", &NetTcpIsClosed);
    regBlocking(registry, "net", "_tcpPeerAddress", &NetTcpPeerAddress);
    regBlocking(registry, "net", "_tcpPeerPort", &NetTcpPeerPort);
    regBlocking(registry, "net", "_tcpSetNoDelay", &NetTcpSetNoDelay);
    regBlocking(registry, "net", "_tcpSetKeepAlive", &NetTcpSetKeepAlive);
    regBlocking(registry, "net", "_tcpSetSendBufferSize", &NetTcpSetSendBufferSize);
    regBlocking(registry, "net", "_tcpSetRecvBufferSize", &NetTcpSetRecvBufferSize);
    reg(registry, "net", "_toBytes", &NetToBytes);
    reg(registry, "net", "_fromBytes", &NetFromBytes);
    //The 18 datagram natives live in net_udp.cpp (registration hook:
    //net_udp.h) — kept out of this TCP-centric entry TU. The 6 ICMP
    //natives likewise live in net_icmp.cpp (hook: net_icmp.h).
    nlang::NetRegisterUdpNatives(registry, reg, regBlocking);
    nlang::NetRegisterIcmpNatives(registry, reg, regBlocking);
    return NLANG_HOST_ABI_VERSION;
}
