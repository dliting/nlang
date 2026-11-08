/*---
    net_icmp.cpp — the ICMP natives (net library design §7): the ping
    convenience path and the raw IcmpSocket arms.

    _ping is one native that parks: the blocking IcmpSendEcho2 helper
    runs on the VM work pool (priority 1, D5 — a slow getaddrinfo in a
    resolve job never delays a ping), and the calling context parks on
    the executor's self-pipe in the resolve-wait shape. The park
    unwinds the native call and re-entry rebuilds the stack, so the
    job record carries the requesting context id: re-entry adopts the
    job it already submitted (FindOwn) instead of minting a new one.
    The helper timeout is always bounded by the echo slice (external
    review I3): a slice that expires while the script deadline still
    allows re-arms with a fresh echo, so one job never holds a pool
    worker longer than one slice and an unlimited ping retransmits
    instead of hanging the pool. NetIcmpEcho's verdicts map through
    the record: -1 (no reply in time) becomes the -2 timeout code, -2
    (unreachable/resolve failure) the -4 platform code.

    The raw IcmpSocket arms follow the UDP discipline: bind mints the
    slot, recvFrom parks on read interest (net_tcp_park.h), the source
    address is stashed on the slot for _icmpLastRecvAddress. On
    Windows the raw socket needs administrator privileges — the bind
    denial surfaces as -4 with the platform text (R10).
---*/
#include "net_icmp.h"

#include "net_jobs.h"
#include "net_platform.h"
#include "net_platform_internal.h"   //g_lastErrorOverride (worker splice)
#include "net_slots.h"
#include "net_state.h"
#include "net_tcp_park.h"

#include <nlang/vm/NativeHost.h>

#include <memory>
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

//Internet checksum (RFC 1071) over the ICMP message. The Windows raw
//ICMP socket requires the application-filled checksum (the helper API
//builds it itself), so _icmpSendTo completes a message whose checksum
//field (bytes 2..3) is still zero.
void IcmpFillChecksum(std::vector<uint8_t>& msg) {
    if (msg.size() < 4 || msg[2] != 0 || msg[3] != 0)
        return;
    uint32_t sum = 0;
    const size_t n = msg.size();
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (static_cast<uint32_t>(msg[i]) << 8) | msg[i + 1];
    if (n & 1)
        sum += static_cast<uint32_t>(msg[n - 1]) << 8;   //odd tail padded
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    const uint16_t ones = static_cast<uint16_t>(~sum);
    msg[2] = static_cast<uint8_t>(ones >> 8);
    msg[3] = static_cast<uint8_t>(ones & 0xFF);
}

//Heap arg record for the pool worker (NativeHost* dangles after the
//call — the state pointer is resolved on submit, net_jobs.cpp shape).
struct IcmpJobArg {
    NetExecState* st;
    int32_t jobId;
    std::string host;
    int timeoutMs;
};

//Pool worker (no GIL): the blocking echo round trip, then the fill.
//The echo data is fixed content — the RTT is the only result the
//script reads. A failure carries its phrase on the record (round-2
//M-3): the resolver's verdict, or an override the platform layer
//armed — the wait native splices both into the caller's store.
void IcmpWorker(void* arg) {
    std::unique_ptr<IcmpJobArg> job(static_cast<IcmpJobArg*>(arg));
    static const char kEchoData[] = "nlang ping";
    int resolveErr = 0;
    const int r = NetIcmpEcho(job->host,
        reinterpret_cast<const uint8_t*>(kEchoData),
        static_cast<int>(sizeof(kEchoData) - 1), job->timeoutMs,
        &resolveErr);
    if (r >= 0) {
        NetFinishIcmp(job->st, job->jobId, r, 0);
        return;
    }
    if (r == -1) {
        NetFinishIcmp(job->st, job->jobId, 0, kNetErrTimeout);
        return;
    }
    std::string errText;
    if (resolveErr != 0) {
        errText = NetResolveErrorText(resolveErr);
    } else if (r == -2) {
        //Either nothing the echo helpers can dial, or the platform
        //layer armed an override on this worker thread — prefer it.
        errText = g_lastErrorOverride.empty()
            ? std::string("host has no usable address")
            : g_lastErrorOverride;
        g_lastErrorOverride.clear();
    }
    NetFinishIcmp(job->st, job->jobId, 0, kNetErrPlatform, errText,
                  resolveErr);
}

//Adopt the job this context already submitted (park re-entry) or mint
//a fresh one, flagging the submit (the ping wait loop's prologue).
int32_t PingMintOrAdopt(NetExecState* st, int self, bool* submit) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    const int32_t jobId = st->m_jobs.FindOwn(kNetJobIcmp, self);
    if (jobId != 0)
        return jobId;
    *submit = true;
    return st->m_jobs.Mint(kNetJobIcmp, self);
}

//Slice expired: release the record and re-arm with a fresh echo (the
//next loop round submits outside the lock; a worker still in flight
//keeps the released record alive until it fills). Caller holds the
//state mutex.
void PingResubmit(NetExecState* st, int32_t* jobId, int self, bool* submit) {
    st->m_jobs.Release(*jobId);
    *jobId = st->m_jobs.Mint(kNetJobIcmp, self);
    *submit = true;
}

//Release the record and stage the verdict. Caller holds the state
//mutex; a worker still in flight keeps the released record alive
//until it fills (net_jobs.h lifetime rule).
void PingReturn(NetExecState* st, int32_t jobId, uint8_t* ret, int value) {
    st->m_jobs.Release(jobId);
    ReturnInt(ret, value);
}

}  //namespace

//Declared in net_icmp.h (unit-tested): the time left until the
//deadline capped by the echo slice (external review I3) — the helper
//timeout is always bounded, so one ping job never holds a pool worker
//longer than one slice; deadline 0 (unlimited) gets one slice.
int IcmpEchoTimeout(long long deadline, long long nowMs) {
    if (deadline <= 0)
        return kIcmpEchoSliceMs;
    const long long left = deadline - nowMs;
    if (left <= 0)
        return 0;
    return left < kIcmpEchoSliceMs
        ? static_cast<int>(left) : kIcmpEchoSliceMs;
}

//Declared in net_icmp.h: only a slice timeout re-arms, and only while
//the deadline still allows (external review I3) — a reply and an
//unreachable verdict are terminal, and an unlimited deadline (0)
//always allows.
//The worker's failure phrase rides the record (round-2 M-3): the
//worker has no NativeHost, so the wait native writes it into THIS
//context's store under the same mutex as the done-check — the
//NetTcpResolveWait splice.
void PingSpliceError(NetExecState* st, int self, const NetJobRecord* rec) {
    if (rec->err == 0 || rec->errText.empty()) return;
    st->m_lastErrorText[self] = rec->errText;
    st->m_lastErrorCode[self] = rec->errCode;
}

bool IcmpShouldResubmit(int err, long long deadline, long long nowMs) {
    return err == kNetErrTimeout && (deadline <= 0 || nowMs < deadline);
}

//ping(host, deadlineMs): mint-or-adopt, submit, park on the self-pipe
//until the record is done (R9 drain discipline: the ack byte is a
//hint, the record under the mutex is the truth). The helper timeout
//is bounded by the echo slice (external review I3): a slice that
//expires while the deadline still allows releases the record and
//re-arms with a fresh echo — retransmission, the shape every ping
//tool uses, and the pool worker is never held longer than one slice,
//so executor teardown and the priority-0 DNS queue stay responsive
//even against a black-hole host. The record is released on every exit
//path — a worker still in flight keeps it alive until it fills
//(net_jobs.h lifetime rule).
void NetPing(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const long long deadline = ArgLong(a, 1);
    const int self = h->currentContextId(h);
    bool submit = false;
    int32_t jobId = PingMintOrAdopt(st, self, &submit);
    const int readFd = st->m_selfPipeFds[0];
    for (;;) {
        if (submit) {
            submit = false;
            h->submitWork(h, &IcmpWorker, new IcmpJobArg{st, jobId, host,
                IcmpEchoTimeout(deadline, h->nowMs(h))},
                st->m_selfPipeFds[1], 1);   //priority 1 (D5 two-level queue)
        }
        NetDrainPipe(readFd);
        std::lock_guard<std::mutex> g(st->m_mutex);
        const NetJobRecord* rec = st->m_jobs.Lookup(jobId);
        if (rec && rec->done) {
            if (IcmpShouldResubmit(rec->err, deadline, h->nowMs(h))) {
                PingResubmit(st, &jobId, self, &submit);
                continue;
            }
            PingSpliceError(st, self, rec);
            PingReturn(st, jobId, ret, rec->err != 0 ? rec->err : rec->rttMs);
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            PingReturn(st, jobId, ret, kNetErrTimeout);
            return;
        }
        if (readFd < 0) {   //self-pair never came up
            PingReturn(st, jobId, ret, kNetErrPlatform);
            return;
        }
        //The done-check and the first-entry registration share one
        //critical section with the worker's fill (final review I3):
        //the ack byte is only a hint and a co-waiter's drain can eat
        //it, so separate sections could park this waiter past a
        //record that was already done (R9 lost-wakeup closure, the
        //NetTcpResolveWait shape). An unlimited deadline parks in
        //bounded slices (external review M9): a lost ack byte or a
        //dead poller thread delays the wake by one slice, never
        //forever.
        const long long parkBy = (deadline > 0) ? deadline
            : h->nowMs(h) + kJobParkSliceMs;
        if (h->parkForIo(h, readFd, NLANG_EV_READ, parkBy, true) == 0)
            return;   //unwound; re-entry adopts the job (FindOwn)
    }
}

//--- raw IcmpSocket ----------------------------------------------------------

//icmpBind(host): SOCK_RAW/IPPROTO_ICMP (Windows: admin — the bind
//denial carries that text through NetLastErrorText), bound to host
//(port is meaningless for ICMP and stays 0).
void NetIcmpBind(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    NetPlatformInit();   //idempotent through a once flag (R4)
    const int fd = NetOpenIcmp();
    if (fd < 0) {
        NetRememberLastError(h);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    if (NetBind(fd, host, 0) != 0) {
        //Read the OS error before the cleanup close (round-2 M-1,
        //the TcpBind shape): a successful closesocket resets it to 0.
        std::string text;
        int code = 0;
        NetLastErrorInfo(text, code);
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
        handle = st->m_slots.Mint(fd, kNetKindIcmp);   }
    ReturnInt(ret, handle);
}

//icmpSendTo(handle, message, host): the full ICMP message (type/code/
//checksum/ID/seq/data) goes out as built by the script; one sendto
//(datagram semantics, no parking). Numeric endpoint only (external
//review Important 2); the guard precedes the checksum fill and the
//handle lookup, so a host name is rejected before any work.
void NetIcmpSendTo(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    NativeArrayView message;
    ArgArray(h, a, 1, &message);
    const std::string host = ArgString(h, a, 2);
    if (!NetIsNumericAddress(host)) { ReturnInt(ret, kNetErrArg); return; }
    std::vector<uint8_t> packed = NetPackBytes(message,
        static_cast<int>(message.elemCount));
    IcmpFillChecksum(packed);   //raw sockets need the app-filled checksum
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, 0, endpoints) || endpoints.empty()) {
        NetRememberLastError(h);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    const NetEndpoint& ep = endpoints.front();
    int fd = -1;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        fd = st->m_slots.FdOf(ArgInt(a, 0));   }
    if (fd < 0) { ReturnInt(ret, kNetErrClosed); return; }
    const int n = NetIcmpSend(fd, packed.data(),
        static_cast<int>(packed.size()), ep.address);
    if (n >= 0) { ReturnInt(ret, n); return; }
    NetRememberLastError(h);
    ReturnInt(ret, kNetErrPlatform);
}

//icmpRecvFrom(handle, buf, deadlineMs): the read-interest park loop
//(NetTcpRead shape) with NetIcmpRecv; the source address is stashed on
//the slot under the same lock as the claim release.
void NetIcmpRecvFrom(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t handle = ArgInt(a, 0);
    NativeArrayView buf;
    ArgArray(h, a, 1, &buf);
    const long long deadline = ArgLong(a, 2);
    const int self = h->currentContextId(h);   //R14
    //Zero-length buf is an argument error (the M-1 recvFrom guard):
    //len-0 consumes a queued message; 0 means an empty datagram.
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
        const int n = NetIcmpRecv(fd, scratch.data(),
            static_cast<int>(buf.elemCount), from);
        if (n >= 0) {
            NetUnpackBytes(buf, scratch.data(), n);
            std::lock_guard<std::mutex> g(st->m_mutex);
            if (NetSocketSlot* slot = st->m_slots.Lookup(handle))
                slot->lastPeerAddress = from;
            if (claimed)
                st->m_slots.ReleaseWaiter(handle, NetDir::Read);
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

//icmpLastRecvAddress(handle): the source of the last recvFrom (the
//empty string — string has no null value, 0.13.4 — when the slot is
//gone; the script reads it right after a successful receive, so the
//slot is live in practice).
void NetIcmpLastRecvAddress(NativeHost* h, uint8_t* ret, const uint8_t* a,
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

//icmpClose(handle): idempotent, the TcpClose contract.
void NetIcmpClose(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
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

//Declared in net_icmp.h: net_native.cpp's module init calls this.
void NetRegisterIcmpNatives(void* registry, RegisterNativeFn reg,
    RegisterNativeBlockingFn regBlocking) {
    regBlocking(registry, "net", "_ping", &NetPing);
    regBlocking(registry, "net", "_icmpBind", &NetIcmpBind);
    regBlocking(registry, "net", "_icmpSendTo", &NetIcmpSendTo);
    regBlocking(registry, "net", "_icmpRecvFrom", &NetIcmpRecvFrom);
    regBlocking(registry, "net", "_icmpLastRecvAddress", &NetIcmpLastRecvAddress);
    regBlocking(registry, "net", "_icmpClose", &NetIcmpClose);
}

}  //namespace nlang
