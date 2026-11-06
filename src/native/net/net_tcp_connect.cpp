/*---
    net_tcp_connect.cpp — the _tcpConnectTry native (net library design
    §7): one endpoint attempt per call, non-blocking connect parked on
    write readiness. Re-entry restarts the native from its first line,
    so the half-open fd survives the park in the per-context
    m_connectInFlight stash (the handle has not reached the script
    yet, so the slot table alone cannot find it). The claim+park
    critical section is shared with the other TCP natives through
    net_tcp_park.h.
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

//First entry of _tcpConnectTry (caller holds the state mutex): open
//+ kick the non-blocking connect. The handle goes into the
//per-context map because re-entry restarts the native from its first
//line and the half-open fd must survive the park (the handle has not
//reached the script yet, so the slot table alone cannot find it).
//Returns 1 = connected now, 0 = in flight (park below), -1 = failed.
int NetConnectKick(NetExecState* st, const std::string& address, int port,
    int self, int32_t* handle) {
    //':' in the text picks the family (v6 literals always carry one;
    //the resolve record keeps address text only).
    const NetFamily family = address.find(':') != std::string::npos
        ? NetFamily::Inet6 : NetFamily::Inet;
    const int fd = NetOpenTcpStream(family);
    if (fd < 0) {
        st->m_lastErrorText = NetLastErrorText();
        *handle = 0;
        return -1;
    }
    *handle = st->m_slots.Mint(fd, kNetKindTcpStream);
    st->m_connectInFlight[self] = *handle;
    const int r = NetConnect(fd, address, port);   //non-blocking
    if (r == 0) {   //connected on the spot
        st->m_connectInFlight.erase(self);
        return 1;
    }
    if (r != kNetInProgress && r != kNetWouldBlock) {
        st->m_lastErrorText = NetLastErrorText();
        st->m_connectInFlight.erase(self);
        st->m_slots.MarkClosed(*handle);   //no waiters: closes + recycles
        return -1;
    }
    return 0;
}

//Re-entry of _tcpConnectTry (caller holds the mutex): the post-park
//verdict via SO_ERROR. Returns 1 = ok, 0 = still in flight (spurious
//wake), -1 = failed, -2 = slot closed during the park.
int NetConnectVerdict(NetExecState* st, int32_t handle, int self,
    bool* claimed) {
    NetSocketSlot* slot = st->m_slots.Lookup(handle);
    if (!slot) {
        st->m_connectInFlight.erase(self);
        st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        return -2;
    }
    *claimed = (slot->writeWaiter == self);
    const int v = NetConnectResult(slot->fd);
    if (v == 0) {   //handshake done: the handle goes to the script
        st->m_connectInFlight.erase(self);
        if (*claimed)
            st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        return 1;
    }
    if (v == kNetInProgress)
        return 0;   //writability probe says still connecting: park again
    if (v < 0) {   //SO_ERROR set: failed, close the half-open stream
        st->m_lastErrorText = NetLastErrorText();
        st->m_connectInFlight.erase(self);
        if (*claimed)
            st->m_slots.ReleaseWaiter(handle, NetDir::Write);
        st->m_slots.MarkClosed(handle);   //waiter-free now: closes + recycles
        return -1;
    }
    return 0;
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
        {
            std::lock_guard<std::mutex> g(st->m_mutex);
            auto it = st->m_connectInFlight.find(self);
            if (it == st->m_connectInFlight.end())
                r = NetConnectKick(st, address, port, self, &handle);
            else {
                handle = it->second;
                r = NetConnectVerdict(st, handle, self, &claimed);
            }
        }
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
