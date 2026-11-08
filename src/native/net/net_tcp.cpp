/*---
    net_tcp.cpp — the TCP data-path natives (net library design §7):
    accept/connect/read/write run their park/retry loop inside the
    native; the VM parks the context and frees the carrier between
    attempts (10k-level concurrency without occupying carriers). The
    connect native lives in net_tcp_connect.cpp; the claim+park
    critical section shared with it is declared in net_tcp_park.h.

    Waiter discipline (R14/R9): the slot's per-direction waiter field
    holds the claiming context id (currentContextId — stable across
    park/re-entry). Claim check, claim set and the parkForIo
    first-entry registration share ONE state-mutex critical section;
    the closer (MarkClosed under the mutex, wakeIoWaiters after the
    unlock) runs on the same mutex — the lost-wakeup window is closed
    on both sides. A call claims its direction at entry (external
    review C1: the GIL is away for the whole native body, so a claim
    only at the park would let two fresh same-direction calls race
    for the bytes); re-entry rebuilds this stack and adopts the claim
    that survived the park (waiter == self). Every exit path releases
    when claimed (R5: the last release performs the deferred
    close). Data always wins over expired bookkeeping: the syscall is
    tried before the deadline is consulted (review focus 1).

    Byte arrays follow the array store's narrow-cell convention (one
    int32 cell per element, low byte = value — same access as
    io.fromBytes): the socket sees packed bytes, so read scatters the
    received run into the cells and write gathers them first.
---*/
#include "net_tcp.h"

#include "net_options.h"
#include "net_platform.h"
#include "net_slots.h"
#include "net_state.h"
#include "net_tcp_park.h"

#include <nlang/runtime/StackDump.h>
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

//Completion tail of a finished write: clear the offset stash and
//release the write claim under one lock (R5 deferred close).
void NetWriteFinished(NetExecState* st, int32_t handle, bool claimed) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    if (NetSocketSlot* slot = st->m_slots.Lookup(handle))
        slot->writeSent = 0;
    if (claimed)
        st->m_slots.ReleaseWaiter(handle, NetDir::Write);
}

//Drains packed into the kernel from *offset: 1 = all bytes handed
//over, 0 = send buffer full (the caller parks for write readiness),
//-1 = hard error (the caller releases the claim and reports -4).
//Progress is stashed on the slot so the next park re-enters at the
//right offset.
int NetSendRun(NetExecState* st, int32_t handle, int fd,
    const std::vector<uint8_t>& packed, int* offset) {
    const int count = static_cast<int>(packed.size());
    while (*offset < count) {
        const int n = NetSend(fd, packed.data() + *offset, count - *offset);
        if (n > 0) {
            *offset += n;
            std::lock_guard<std::mutex> g(st->m_mutex);
            if (NetSocketSlot* slot = st->m_slots.Lookup(handle))
                slot->writeSent = *offset;   //survive the next park
            continue;
        }
        if (n == kNetWouldBlock || n == 0)
            return 0;   //send buffer full
        return -1;   //EPIPE: connection lost
    }
    return 1;
}

}  //namespace

//Declared in net_tcp_park.h: the connect TU shares this critical
//section; it stays here beside the waiter fields it manipulates.
NetParkOutcome NetClaimAndPark(NetExecState* st, int32_t handle, NetDir dir,
    int self, long long deadline, NativeHost* h) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    NetSocketSlot* slot = st->m_slots.Lookup(handle);
    if (!slot)
        return kNetParkClosed;   //caller releases its own claim after
    const int waiter = (dir == NetDir::Read) ? slot->readWaiter
        : (dir == NetDir::Write) ? slot->writeWaiter : slot->acceptWaiter;
    if (waiter != 0 && waiter != self)
        return kNetParkConcurrent;
    st->m_slots.SetWaiter(handle, dir, self);
    const unsigned ev = (dir == NetDir::Write) ? NLANG_EV_WRITE : NLANG_EV_READ;
    if (h->parkForIo(h, slot->fd, ev, deadline, deadline > 0) == 0)
        return kNetParkUnwound;
    return kNetParkReplay;
}

//Releases the direction's claim if this call holds it (R9: table
//access under the state mutex; R5: the last release closes the fd).
//Declared in net_tcp_park.h: shared by every parking native (the UDP
//receive arms run the same discipline).
void NetRelease(NetExecState* st, int32_t handle, NetDir dir, bool claimed) {
    if (!claimed)
        return;
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_slots.ReleaseWaiter(handle, dir);
}

//Claims the direction for this call (or adopts the claim that
//survived a park) and reads the slot's fd, under the mutex.
//kNetAdoptGone when the slot is gone: the direction's waiter is
//cleared unconditionally — a claim there is either this call's own
//pre-park claim (releasing it lets the R5 deferred close proceed) or
//a foreign one the closer already woke (that waiter re-enters through
//the same Gone branch without touching the fd, so closing the fd now
//cannot race a parked call). kNetAdoptConcurrent when a foreign
//context holds the direction: rejecting at entry is what makes the
//D11 "foreign waiters stay out while the claim is set" invariant hold
//for the callers' per-direction state (final review I2: a fresh write
//that adopted through here reset the parked writer's writeSent).
NetAdoptOutcome NetAdoptClaim(NetExecState* st, int32_t handle, NetDir dir,
    int self, int* fd, bool* claimed, bool* freshCall) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    NetSocketSlot* slot = st->m_slots.Lookup(handle);
    if (!slot) {
        st->m_slots.ReleaseWaiter(handle, dir);
        return kNetAdoptGone;
    }
    *fd = slot->fd;
    const int waiter = (dir == NetDir::Read) ? slot->readWaiter
        : (dir == NetDir::Write) ? slot->writeWaiter
        : slot->acceptWaiter;
    if (waiter != 0 && waiter != self)
        return kNetAdoptConcurrent;
    const bool adopted = (waiter == self);   //re-entry keeps its stash
    //External review C1: claim at entry, not at the park. The
    //blocking arm releases the GIL for the whole native body, so two
    //fresh same-direction calls run concurrently until one parks —
    //an unclaimed window let the second reset the first's
    //per-direction state (writeSent) and split or duplicate the
    //stream. Claiming here makes the manual's one-operation-per-
    //direction contract hold unconditionally: the second fresh
    //caller sees a foreign waiter and is rejected before touching
    //anything. Re-entry (waiter == self) adopts the claim that
    //survived the park; every exit path releases when claimed.
    st->m_slots.SetWaiter(handle, dir, self);
    *claimed = true;
    if (freshCall)
        *freshCall = !adopted;
    return kNetAdoptOk;
}

//Shared park tail: true = the caller must return (unwound: no value
//staged; closed/concurrent: ret filled with the R10 code after
//releasing this call's claim). False = replay the operation.
bool NetParkTail(NetExecState* st, int32_t handle, NetDir dir,
    NetParkOutcome outcome, bool claimed, uint8_t* ret) {
    if (outcome == kNetParkUnwound)
        return true;
    if (outcome != kNetParkReplay) {
        NetRelease(st, handle, dir, claimed);
        ReturnInt(ret, outcome == kNetParkClosed
            ? kNetErrClosed : kNetErrConcurrent);
        return true;
    }
    return false;
}

//Gathers count byte-array cells (low bytes, view offset/stride aware)
//into a packed send buffer. The script layer rejects count > length
//(final review M2); the assert is the tripwire for future callers.
std::vector<uint8_t> NetPackBytes(const NativeArrayView& view, int count) {
    NLANG_ASSERT(count >= 0 && count <= view.elemCount);
    const auto* cells = static_cast<const int32_t*>(view.data);
    const int64_t o = view.offset, step = view.strides[0];
    std::vector<uint8_t> packed(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        packed[static_cast<size_t>(i)] = static_cast<unsigned char>(
            cells[o + static_cast<int64_t>(i) * step]);
    return packed;
}

//Scatters a received packed run into byte-array cells (low bytes,
//view offset/stride aware).
void NetUnpackBytes(const NativeArrayView& view, const uint8_t* bytes,
    int count) {
    auto* cells = static_cast<int32_t*>(view.data);
    const int64_t o = view.offset, step = view.strides[0];
    for (int i = 0; i < count; ++i)
        cells[o + static_cast<int64_t>(i) * step] =
            bytes[static_cast<size_t>(i)];
}

//--- read ------------------------------------------------------------------

//_tcpRead(handle, buf, deadlineMs): try recv, then the deadline, then
//claim+park. Returns bytes read (0 = EOF) or an R10 code. A
//zero-length buf is rejected as an argument error (external review
//M12): recv(fd, ptr, 0) returns 0, which this contract reserves for
//EOF — the degenerate call must not masquerade as end of stream. The
//arg guard precedes the slot state, the numeric-only discipline.
void NetTcpRead(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView buf;
    ArgArray(h, a, 1, &buf);
    const long long deadline = ArgLong(a, 2);
    const int self = h->currentContextId(h);   //R14
    if (buf.elemCount == 0) { ReturnInt(ret, kNetErrArg); return; }
    std::vector<uint8_t> scratch(static_cast<size_t>(buf.elemCount));
    bool claimed = false;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Read, self, &fd, &claimed);
        if (adopt != kNetAdoptOk) {
            //Concurrent: claimed stayed false — this call never held
            //the direction, so there is nothing to release.
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        const int n = NetRecv(fd, scratch.data(),
            static_cast<int>(buf.elemCount));
        if (n >= 0) {   //data or EOF (n == 0)
            NetUnpackBytes(buf, scratch.data(), n);
            NetRelease(st, handle, NetDir::Read, claimed);
            ReturnInt(ret, n);
            return;
        }
        if (n != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Read, claimed);
            NetRememberLastError(h);   //lastErrorText under the mutex
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

//The write payload's starting offset: a fresh entry restarts at
//zero (clearing any stash a timed-out or failed write left behind),
//a park re-entry resumes the stashed progress (I-1 — the stash
//survives parks within one call and dies with it).
int NetWriteOffset(NetExecState* st, int32_t handle, bool fresh) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    NetSocketSlot* slot = st->m_slots.Lookup(handle);
    if (!slot)
        return 0;
    if (fresh)
        slot->writeSent = 0;
    return slot->writeSent;
}

//--- write -----------------------------------------------------------------

//_tcpWrite(handle, data, count, deadlineMs): full write — loops until
//all count bytes are handed to the kernel. The bytes already sent
//live on the slot (writeSent) because re-entry rebuilds this stack.
//Returns the count sent or an R10 code (EPIPE → -4, review focus 3).
void NetTcpWrite(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView data;
    ArgArray(h, a, 1, &data);
    const int count = ArgInt(a, 2);
    const long long deadline = ArgLong(a, 3);
    const int self = h->currentContextId(h);
    const std::vector<uint8_t> packed = NetPackBytes(data, count);
    bool claimed = false;
    bool fresh = false;
    int offset = 0;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Write, self, &fd, &claimed, &fresh);
        if (adopt != kNetAdoptOk) {
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        offset = NetWriteOffset(st, handle, fresh);
        const int run = NetSendRun(st, handle, fd, packed, &offset);
        if (run < 0) {
            NetRelease(st, handle, NetDir::Write, claimed);
            NetRememberLastError(h);   //EPIPE: connection lost
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (run > 0) {   //all bytes handed to the kernel
            NetWriteFinished(st, handle, claimed);
            ReturnInt(ret, offset);
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

//--- accept ----------------------------------------------------------------

//_tcpAccept(listener, deadlineMs): accept on the listener's read
//interest; the accepted fd is minted into a new stream slot. Returns
//the new handle or an R10 code.
void NetTcpAccept(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    const long long deadline = ArgLong(a, 1);
    const int self = h->currentContextId(h);
    bool claimed = false;
    for (;;) {
        int fd = -1;
        const NetAdoptOutcome adopt = NetAdoptClaim(st, handle,
            NetDir::Accept, self, &fd, &claimed);
        if (adopt != kNetAdoptOk) {
            ReturnInt(ret, NetAdoptErrCode(adopt));
            return;
        }
        const int peer = NetAccept(fd);   //non-blocking
        if (peer >= 0) {
            NetRelease(st, handle, NetDir::Accept, claimed);
            int32_t newHandle = 0;
            {
                std::lock_guard<std::mutex> g(st->m_mutex);
                newHandle = st->m_slots.Mint(peer, kNetKindTcpStream);
            }
            ReturnInt(ret, newHandle);
            return;
        }
        if (peer != kNetWouldBlock) {
            NetRelease(st, handle, NetDir::Accept, claimed);
            NetRememberLastError(h);
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            NetRelease(st, handle, NetDir::Accept, claimed);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (NetParkTail(st, handle, NetDir::Accept,
                NetClaimAndPark(st, handle, NetDir::Accept, self, deadline, h),
                claimed, ret))
            return;
    }
}

//--- remaining contracts (step 6) ------------------------------------------

//_tcpShutdownWrite(handle): half-close (FIN); the read side stays
//usable. 0, -1 closed, -4 platform.
void NetTcpShutdownWrite(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    int fd = -1;
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));
    }
    if (fd < 0) {
        ReturnInt(ret, kNetErrClosed);
        return;
    }
    if (NetShutdownWrite(fd) == 0) {
        ReturnInt(ret, 0);
        return;
    }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

//_tcpIsClosed(handle): slot missing or flagged closed → 1.
void NetTcpIsClosed(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    ReturnInt(ret, st->m_slots.Lookup(ArgInt(a, 0)) ? 0 : 1);
}

//_tcpPeerAddress(handle): connected-peer text (v4-mapped unwrapped);
//the empty string (string has no null value — 0.13.4) on a closed slot
//or getpeername failure.
void NetTcpPeerAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    const NetSocketSlot* slot = st->m_slots.Lookup(ArgInt(a, 0));
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

//_tcpPeerPort(handle): connected-peer port; R10 code on failure.
void NetTcpPeerPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    const NetSocketSlot* slot = st->m_slots.Lookup(ArgInt(a, 0));
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

//--- options ---------------------------------------------------------------

//The setter shape itself lives in net_options.cpp (shared with the
//UDP option natives).

//_tcpSetNoDelay(handle, on): TCP_NODELAY. 0, -1 closed, -4 platform.
void NetTcpSetNoDelay(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetApplyFlag(h, ret, a, NetSetNoDelay);
}

//_tcpSetKeepAlive(handle, on): SO_KEEPALIVE.
void NetTcpSetKeepAlive(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetApplyFlag(h, ret, a, NetSetKeepAlive);
}

//_tcpSetSendBufferSize(handle, bytes): SO_SNDBUF (the kernel may clamp).
void NetTcpSetSendBufferSize(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetApplyInt(h, ret, a, NetSetSendBuffer);
}

//_tcpSetRecvBufferSize(handle, bytes): SO_RCVBUF (the kernel may clamp).
void NetTcpSetRecvBufferSize(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetApplyInt(h, ret, a, NetSetRecvBuffer);
}

}  //namespace nlang
