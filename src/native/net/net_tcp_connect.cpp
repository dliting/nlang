/*---
    net_tcp_connect.cpp — the _tcpConnectTry native (net library design
    §7): one endpoint attempt per call, non-blocking connect parked on
    write readiness. Re-entry restarts the native from its first line,
    so the half-open fd survives the park in the per-context
    m_connectInFlight stash (the handle has not reached the script
    yet, so the slot table alone cannot find it). The syscalls
    (socket/connect/zero-timeout select/getsockopt) run OUTSIDE the
    state mutex (external review I7): serializing the 10k-connect
    fan-out behind the executor-wide lock cancels the M:N scheduler's
    benefit, so the lock covers only the µs-fast table bookkeeping and
    the generation-coded handle is revalidated after every
    re-acquisition. The claim+park critical section is shared with the
    other TCP natives through net_tcp_park.h.
---*/
#include "net_tcp.h"

#include "net_platform.h"
#include "net_slots.h"
#include "net_state.h"
#include "net_tcp_park.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>

using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::ArgString;
using nlang::native::ReturnInt;

namespace nlang {
namespace {

//First entry of _tcpConnectTry: open + kick the non-blocking connect
//outside the state mutex (I7 — a non-blocking connect can still stall
//briefly on route lookup or ARP, and the 10k fan-out must not queue
//behind the executor-wide lock). The fd is local until the
//bookkeeping lock mints the slot and stashes it atomically for
//re-entry; an immediate failure closes the fd without ever minting,
//so a connect-failure storm never churns the slot table. Returns
//1 = connected now, 0 = in flight (park below), -1 = failed.
int NetConnectKick(NativeHost* h, const std::string& address, int port,
    int self, int32_t* handle) {
    NetExecState* st = NetState(h);
    //':' in the text picks the family (v6 literals always carry one;
    //the resolve record keeps address text only).
    const NetFamily family = address.find(':') != std::string::npos
        ? NetFamily::Inet6 : NetFamily::Inet;
    const int fd = NetOpenTcpStream(family);
    if (fd < 0) {
        NetRememberLastError(h);   //takes the lock — not held here
        *handle = 0;
        return -1;
    }
    const int r = NetConnect(fd, address, port);   //non-blocking
    if (r != 0 && r != kNetInProgress && r != kNetWouldBlock) {
        NetRememberLastError(h);   //OS text still current on this thread
        NetClose(fd);
        *handle = 0;
        return -1;
    }
    std::lock_guard<std::mutex> g(st->m_mutex);
    *handle = st->m_slots.Mint(fd, kNetKindTcpStream);
    if (r != 0)
        st->m_connectInFlight[self] = *handle;   //park below re-enters
    return r == 0 ? 1 : 0;
}

//Re-entry of _tcpConnectTry: snapshot the fd under the lock, probe
//the connect completion (zero-timeout select + getsockopt) outside
//it (I7), then re-take the lock to commit the verdict. The handle is
//generation-coded: a slot released during the probe window re-Lookups
//to null and the verdict becomes the closed code; the waiter claim is
//recomputed at commit rather than carried across the probe. The
//failure text is read on the probing thread while the OS override is
//still current. Returns 1 = ok, 0 = still in flight (spurious wake),
//-1 = failed, -2 = slot closed during the park.
int NetConnectVerdict(NetExecState* st, int32_t handle, int self,
    bool* claimed) {
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        if (const NetSocketSlot* slot = st->m_slots.Lookup(handle))
            fd = slot->fd;   }
    int v = kNetInProgress;
    std::string errText;
    if (fd >= 0) {
        v = NetConnectResult(fd);   //outside the lock (I7)
        if (v < 0)
            errText = NetLastErrorText();
    }
    std::lock_guard<std::mutex> g(st->m_mutex);
    const NetSocketSlot* slot = st->m_slots.Lookup(handle);
    if (!slot) {   //slot died during the probe window
        st->m_connectInFlight.erase(self);
        st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        return -2;
    }
    *claimed = (slot->writeWaiter == self);
    if (v == 0) {   //handshake done: the handle goes to the script
        st->m_connectInFlight.erase(self);
        if (*claimed)
            st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        return 1;
    }
    if (v < 0) {   //SO_ERROR set: failed, close the half-open stream
        st->m_lastErrorText[self] = errText;
        st->m_connectInFlight.erase(self);
        if (*claimed)
            st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        st->m_slots.MarkClosed(handle);   //waiter-free now: closes + recycles
        return -1;
    }
    return 0;   //writability probe says still connecting: park again
}

//Drops the per-context stash and closes a failed or timed-out connect
//slot (claim released first so the R5 close sees no waiters).
void NetAbandonConnect(NetExecState* st, int32_t handle, int self,
    bool claimed) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_connectInFlight.erase(self);
    if (claimed)
        st->m_slots.ReleaseWaiter(handle, NetDir::Write);
    st->m_slots.MarkClosed(handle);   //waiter-free now: closes + recycles
}

}  //namespace

//_tcpConnectTry(address, port, deadlineMs): one endpoint attempt with
//a non-blocking connect parked on write readiness; the deadline check
//has the read shape (review focus 1: a completed handshake wins over
//an expired deadline). Returns the new handle or an R10 code.
void NetTcpConnectTry(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string address = ArgString(h, a, 0);
    const int port = ArgInt(a, 1);
    const long long deadline = ArgLong(a, 2);
    const int self = h->currentContextId(h);
    for (;;) {
        int32_t handle = 0;
        bool claimed = false;
        int r = 0;
        {   std::lock_guard<std::mutex> g(st->m_mutex);
            auto it = st->m_connectInFlight.find(self);
            if (it != st->m_connectInFlight.end())
                handle = it->second;   }
        if (handle != 0)
            r = NetConnectVerdict(st, handle, self, &claimed);
        else
            r = NetConnectKick(h, address, port, self, &handle);
        if (r == 1) { ReturnInt(ret, handle); return; }
        if (r == -1) { ReturnInt(ret, kNetErrPlatform); return; }
        if (r == -2) { ReturnInt(ret, kNetErrClosed); return; }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            NetAbandonConnect(st, handle, self, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        const NetParkOutcome outcome =
            NetClaimAndPark(st, handle, NetDir::Write, self, deadline, h);
        if (outcome == kNetParkUnwound)
            return;
        if (outcome == kNetParkClosed) {
            std::lock_guard<std::mutex> g(st->m_mutex);
            st->m_connectInFlight.erase(self);
            st->m_slots.ReleaseWaiter(handle, NetDir::Write);
            ReturnInt(ret, kNetErrClosed);
            return;
        }
        if (outcome == kNetParkConcurrent) {
            ReturnInt(ret, kNetErrConcurrent);   //unreachable: ours
            return;
        }
    }
}

}  //namespace nlang
