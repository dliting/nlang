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
    NetIcmpEcho's verdicts map through the record: -1 (no reply in
    time) becomes the -2 timeout code, -2 (unreachable/resolve
    failure) the -4 platform code.

    The raw IcmpSocket arms follow the UDP discipline: bind mints the
    slot, recvFrom parks on read interest (net_tcp_park.h), the source
    address is stashed on the slot for _icmpLastRecvAddress. On
    Windows the raw socket needs administrator privileges — the bind
    denial surfaces as -4 with the platform text (R10).
---*/
#include "net_icmp.h"

#include "net_jobs.h"
#include "net_platform.h"
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

//Echo timeout for an unlimited ping (deadline 0): ~24.8 days in the
//helper's DWORD milliseconds — effectively infinite, and the context
//parks without a deadline anyway.
constexpr int kIcmpInfiniteTimeoutMs = 0x7FFFFFFF;

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

//Echo timeout for the helper: the time left until the deadline, clamped
//to the helper's DWORD range; deadline 0 means unlimited.
int IcmpEchoTimeout(long long deadline, long long nowMs) {
    if (deadline <= 0)
        return kIcmpInfiniteTimeoutMs;
    const long long left = deadline - nowMs;
    if (left <= 0)
        return 0;
    return left < kIcmpInfiniteTimeoutMs
        ? static_cast<int>(left) : kIcmpInfiniteTimeoutMs;
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
//script reads.
void IcmpWorker(void* arg) {
    std::unique_ptr<IcmpJobArg> job(static_cast<IcmpJobArg*>(arg));
    static const char kEchoData[] = "nlang ping";
    const int r = NetIcmpEcho(job->host,
        reinterpret_cast<const uint8_t*>(kEchoData),
        static_cast<int>(sizeof(kEchoData) - 1), job->timeoutMs);
    if (r >= 0) {
        NetFinishIcmp(job->st, job->jobId, r, 0);
        return;
    }
    NetFinishIcmp(job->st, job->jobId, 0,
        r == -1 ? kNetErrTimeout : kNetErrPlatform);
}

}  //namespace

//ping(host, deadlineMs): mint-or-adopt, submit once, park on the
//self-pipe until the record is done (R9 drain discipline: the ack
//byte is a hint, the record under the mutex is the truth). The record
//is released on every exit path — a worker still in flight keeps it
//alive until it fills (net_jobs.h lifetime rule).
void NetPing(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const long long deadline = ArgLong(a, 1);
    const int self = h->currentContextId(h);
    int32_t jobId = 0;
    bool submit = false;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        jobId = st->m_jobs.FindOwn(kNetJobIcmp, self);
        if (jobId == 0) {
            jobId = st->m_jobs.Mint(kNetJobIcmp, self);
            submit = true;
        }   }
    if (submit) {
        h->submitWork(h, &IcmpWorker, new IcmpJobArg{st, jobId, host,
            IcmpEchoTimeout(deadline, h->nowMs(h))},
            st->m_selfPipeFds[1], 1);   //priority 1 (D5 two-level queue)
    }
    const int readFd = st->m_selfPipeFds[0];
    for (;;) {
        NetDrainPipe(readFd);
        {   std::lock_guard<std::mutex> g(st->m_mutex);
            const NetJobRecord* rec = st->m_jobs.Lookup(jobId);
            if (rec && rec->done) {
                const int r = rec->err != 0 ? rec->err : rec->rttMs;
                st->m_jobs.Release(jobId);
                ReturnInt(ret, r);
                return;
            }   }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            std::lock_guard<std::mutex> g(st->m_mutex);
            st->m_jobs.Release(jobId);
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (readFd < 0) {   //self-pair never came up
            std::lock_guard<std::mutex> g(st->m_mutex);
            st->m_jobs.Release(jobId);
            ReturnInt(ret, kNetErrPlatform);
            return;
        }
        if (h->parkForIo(h, readFd, NLANG_EV_READ, deadline,
                deadline > 0) == 0)
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
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    if (NetBind(fd, host, 0) != 0) {
        NetClose(fd);
        NetRememberLastError(st);
        ReturnInt(ret, kNetErrPlatform);
        return;
    }
    int32_t handle = 0;
    {   std::lock_guard<std::mutex> g(st->m_mutex);
        handle = st->m_slots.Mint(fd, kNetKindIcmp);   }
    ReturnInt(ret, handle);
}

//icmpSendTo(handle, message, host): the full ICMP message (type/code/
//checksum/ID/seq/data) goes out as built by the script; resolve then
//one sendto (datagram semantics, no parking).
void NetIcmpSendTo(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    NativeArrayView message;
    ArgArray(h, a, 1, &message);
    const std::string host = ArgString(h, a, 2);
    std::vector<uint8_t> packed = NetPackBytes(message,
        static_cast<int>(message.elemCount));
    IcmpFillChecksum(packed);   //raw sockets need the app-filled checksum
    std::vector<NetEndpoint> endpoints;
    if (!NetResolve(host, 0, endpoints) || endpoints.empty()) {
        NetRememberLastError(st);
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
    NetRememberLastError(st);
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
    std::vector<uint8_t> scratch(static_cast<size_t>(buf.elemCount));
    bool claimed = false;
    for (;;) {
        int fd = -1;
        if (!NetAdoptClaim(st, handle, NetDir::Read, self, &fd, &claimed)) {
            ReturnInt(ret, kNetErrClosed);
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

//icmpLastRecvAddress(handle): the source of the last recvFrom (null
//sentinel 0 when the slot is gone).
void NetIcmpLastRecvAddress(NativeHost* h, uint8_t* ret, const uint8_t* a,
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
