/*---
    net_native.cpp — the nlang_net module entry (net library design §6):
    the internal primitives the net.n script surface calls (underscore
    names are module-private by convention; the public face is script).
    All six register on the blocking channel — the r1 model releases the
    GIL around blocking natives, so the slot table and the last-error
    text are guarded by NetExecState::m_mutex (plan ruling R9) and the
    lock is scoped to the state accesses only (a slow getaddrinfo in
    bind must not serialize other carriers' calls). Failures return the
    R10 error codes; -4 paths record the platform text for
    _lastErrorText. Exception mapping lives in the script (_throwNet).
---*/
#include "net_platform.h"
#include "net_state.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>
#include <vector>

using nlang::native::ArgInt;
using nlang::native::ArgString;
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

namespace {

//TcpListener accept-queue depth (the backlog every listener API hides
//behind a constant; 64 matches the common library default).
constexpr int kNetListenBacklog = 64;

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
    return NLANG_HOST_ABI_VERSION;
}
