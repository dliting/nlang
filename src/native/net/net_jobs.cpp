/*---
    net_jobs.cpp — the job table mechanics, the pool worker form and
    the resolve natives (net_jobs.h). The wait loop parks on the
    executor's self-pipe read end: ack bytes are wake hints, the record
    under the mutex is the truth (R9 drain discipline — level-triggered
    re-check, no spin, and one waiter's drain can never strand another:
    either done is visible in the critical section or the ack byte was
    written after this waiter's interest registration).
---*/
#include "net_jobs.h"

#include "net_platform.h"
#include "net_state.h"

#include <nlang/vm/NativeHost.h>

#include <memory>
#include <mutex>
#include <utility>

using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::ArgString;
using nlang::native::ReturnInt;
using nlang::native::ReturnString;

namespace nlang {

//--- job table -------------------------------------------------------------

int32_t NetJobTable::Mint(int kind, int owner) {
    int32_t index = 0;
    if (!m_freeList.empty()) {
        index = m_freeList.back();
        m_freeList.pop_back();
    } else {
        index = static_cast<int32_t>(m_records.size());
        m_records.emplace_back();
        m_inUse.push_back(0);
    }
    m_records[index] = NetJobRecord{};
    m_records[index].kind = kind;
    m_records[index].owner = owner;
    m_inUse[index] = 1;
    return index + 1;
}

NetJobRecord* NetJobTable::Lookup(int32_t jobId) {
    const int32_t index = jobId - 1;
    if (index < 0 || index >= static_cast<int32_t>(m_records.size())
        || !m_inUse[index])
        return nullptr;
    return &m_records[index];
}

void NetJobTable::MarkDone(int32_t jobId,
    std::vector<std::string>&& endpoints, int err, std::string errText,
    int errCode) {
    if (NetJobRecord* rec = Lookup(jobId)) {
        rec->endpoints = std::move(endpoints);
        rec->err = err;
        rec->errText = std::move(errText);
        rec->errCode = errCode;
        rec->done = true;
        FreeIfBoth(jobId);   //release may have come first (net_jobs.h
    }                        //lifetime rule: free on the second flag)
}

//Only records the owner has not released are adoptable (final review
//I4): a released record with the worker still in flight stays in use
//until the worker fills it, but its waiter already left — re-adopting
//it would hand a fresh call the stale job's result.
int32_t NetJobTable::FindOwn(int kind, int owner) const {
    for (size_t i = 0; i < m_records.size(); ++i)
        if (m_inUse[i] && !m_records[i].released
            && m_records[i].kind == kind && m_records[i].owner == owner)
            return static_cast<int32_t>(i) + 1;
    return 0;
}

void NetJobTable::MarkDoneIcmp(int32_t jobId, int rttMs, int err,
                               std::string errText, int errCode) {
    if (NetJobRecord* rec = Lookup(jobId)) {
        rec->rttMs = rttMs;
        rec->err = err;
        rec->errText = std::move(errText);   //ping splice (round-2 M-3)
        rec->errCode = errCode;
        rec->done = true;
        FreeIfBoth(jobId);   //release may have come first (net_jobs.h
    }                        //lifetime rule: free on the second flag)
}

void NetJobTable::Release(int32_t jobId) {
    if (NetJobRecord* rec = Lookup(jobId)) {
        rec->released = true;
        FreeIfBoth(jobId);
    }
}

void NetJobTable::FreeIfBoth(int32_t jobId) {
    const int32_t index = jobId - 1;
    if (index < 0 || index >= static_cast<int32_t>(m_records.size())
        || !m_inUse[index])
        return;
    const NetJobRecord& rec = m_records[index];
    if (!rec.done || !rec.released)
        return;
    m_inUse[index] = 0;
    m_freeList.push_back(index);
}

int NetJobTable::InFlightCount() const {
    int count = 0;
    for (size_t i = 0; i < m_records.size(); ++i)
        if (m_inUse[i] && !m_records[i].done)
            ++count;
    return count;
}

//--- worker offload ----------------------------------------------------------

namespace {

//Heap arg record for the pool worker. NativeHost* is a per-call stack
//object (dangling once the call returns — R9), so the worker carries
//the state pointer resolved on submit instead; the pool outlives the
//call but the dtor's cv wait keeps the state alive for every in-flight
//job.
struct ResolveJobArg {
    NetExecState* st;
    int32_t jobId;
    std::string host;
    int port;
};

//Pool worker (no GIL): the blocking getaddrinfo, then the fill. The
//ack byte is the pool's job after fn returns (ackFd = self-pipe).
void ResolveWorker(void* arg) {
    std::unique_ptr<ResolveJobArg> job(static_cast<ResolveJobArg*>(arg));
    std::vector<NetEndpoint> endpoints;
    int resolveErr = 0;
    const bool ok = NetResolve(job->host, job->port, endpoints, &resolveErr);
    std::vector<std::string> texts;
    for (const NetEndpoint& ep : endpoints)
        texts.push_back(ep.address);
    //The failure phrase rides the record: the worker has no
    //NativeHost, so the per-context error store is out of reach here;
    //the wait native splices it in on the caller's context (M-2).
    NetFinishResolve(job->st, job->jobId, std::move(texts),
        ok ? 0 : kNetErrPlatform,
        ok ? std::string() : NetResolveErrorText(resolveErr), resolveErr);
}

}  //namespace

//Declared in net_jobs.h: shared by the resolve wait and the ping wait.
void NetDrainPipe(int readFd) {
    if (readFd < 0)
        return;
    uint8_t scratch[64];
    for (;;) {
        const int n = NetRecv(readFd, scratch, static_cast<int>(sizeof(scratch)));
        if (n <= 0)
            return;   //would-block (or EOF: the pair is gone)
    }
}

void NetFinishResolve(NetExecState* st, int32_t jobId,
    std::vector<std::string>&& endpoints, int err, std::string errText,
    int errCode) {
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        st->m_jobs.MarkDone(jobId, std::move(endpoints), err,
            std::move(errText), errCode);
    }
    st->m_jobsDoneCv.notify_all();   //teardown wait (R9)
}

void NetFinishIcmp(NetExecState* st, int32_t jobId, int rttMs, int err,
                   std::string errText, int errCode) {
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        st->m_jobs.MarkDoneIcmp(jobId, rttMs, err, std::move(errText),
                                errCode);
    }
    st->m_jobsDoneCv.notify_all();   //teardown wait (R9)
}

//--- resolve natives ---------------------------------------------------------

//_tcpResolveStart(host, port): mints the job and either fills it on
//the spot (numeric literal — no pool round trip for a lookup that
//cannot block, ack written directly so the wait path stays identical)
//or submits it to the pool (priority 0). Returns the jobId.
void NetTcpResolveStart(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const std::string host = ArgString(h, a, 0);
    const int port = ArgInt(a, 1);
    int32_t jobId = 0;
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        jobId = st->m_jobs.Mint(kNetJobResolve);
    }
    if (NetIsNumericAddress(host)) {
        std::vector<NetEndpoint> endpoints;
        const bool ok = NetResolve(host, port, endpoints);
        std::vector<std::string> texts;
        for (const NetEndpoint& ep : endpoints)
            texts.push_back(ep.address);
        NetFinishResolve(st, jobId, std::move(texts), ok ? 0 : kNetErrPlatform);
        if (st->m_selfPipeFds[1] >= 0) {
            const uint8_t ack = 1;
            //Errors ignored: a full or closed pipe only delays a wake
            //the level-triggered re-check still delivers.
            NetSend(st->m_selfPipeFds[1], &ack, 1);
        }
    } else {
        h->submitWork(h, &ResolveWorker,
            new ResolveJobArg{st, jobId, host, port},
            st->m_selfPipeFds[1], 0);
    }
    ReturnInt(ret, jobId);
}

//_tcpResolveWait(jobId, deadlineMs): parks on the self-pipe until the
//record is done (count or R10 err), the deadline passes (-2) or the
//pipe never came up (-4). Never releases the record — the script's
//_tcpResolveRelease does that on every path (idempotent).
void NetTcpResolveWait(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    const int32_t jobId = ArgInt(a, 0);
    const long long deadline = ArgLong(a, 1);
    const int readFd = st->m_selfPipeFds[0];
    for (;;) {
        NetDrainPipe(readFd);
        std::lock_guard<std::mutex> g(st->m_mutex);
        const NetJobRecord* rec = st->m_jobs.Lookup(jobId);
        if (rec && rec->done) {
            if (rec->err != 0 && !rec->errText.empty()) {
                //Splice the worker's failure phrase into THIS context's
                //error store (M-2): the worker could not write it — it
                //has no NativeHost — and the script's _throwNet reads
                //it right after the -4 return, like every other path.
                const int self = h->currentContextId(h);
                st->m_lastErrorText[self] = rec->errText;
                st->m_lastErrorCode[self] = rec->errCode;
            }
            ReturnInt(ret, rec->err != 0 ? rec->err
                : static_cast<int>(rec->endpoints.size()));
            return;
        }
        if (deadline > 0 && h->nowMs(h) >= deadline) {
            ReturnInt(ret, kNetErrTimeout);
            return;
        }
        if (readFd < 0) {
            ReturnInt(ret, kNetErrPlatform);   //self-pair never came up
            return;
        }
        //The first-entry registration shares the mutex with the
        //worker's fill: either done is visible now, or the ack byte
        //lands after the registration (R9 lost-wakeup closure). An
        //unlimited deadline parks in bounded slices (external review
        //M9): the ack write can be lost (a co-waiter's drain, a full
        //pipe, a dead poller thread), and the slice re-check bounds
        //the delay instead of sleeping forever.
        const long long parkBy = (deadline > 0) ? deadline
            : h->nowMs(h) + kJobParkSliceMs;
        if (h->parkForIo(h, readFd, NLANG_EV_READ, parkBy, true) == 0)
            return;   //unwound; the re-entry re-checks
    }
}

//_tcpResolveEndpoint(jobId, index): the i-th address text; null
//outside the filled result.
void NetTcpResolveEndpoint(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    const NetJobRecord* rec = st->m_jobs.Lookup(ArgInt(a, 0));
    const int index = ArgInt(a, 1);
    if (!rec || !rec->done || index < 0
        || index >= static_cast<int>(rec->endpoints.size())) {
        ReturnInt(ret, 0);   //null string
        return;
    }
    ReturnString(h, ret, rec->endpoints[index]);
}

//_tcpResolveRelease(jobId): idempotent; frees the record once the
//worker has also filled it (a late ack then converges on its own).
void NetTcpResolveRelease(NativeHost* h, uint8_t* ret, const uint8_t* a, int) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_jobs.Release(ArgInt(a, 0));
    ReturnInt(ret, 0);
}

}  //namespace nlang
