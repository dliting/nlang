/*---
    net_jobs — the blocking-OS-call offload of the net module (design
    §5/D5): DNS resolve and (task 11) ICMP echo jobs are submitted to
    the VM's generic native worker pool via host->submitWork — the pool
    is VM infrastructure (r1补注4); the module keeps only the job table
    and the self-pipe ack. ICMP uses priority 1 so a slow getaddrinfo
    never delays a ping (two-level queue, libuv precedent) without any
    module-private threads. Job records live in the per-executor
    NetExecState; the requesting context parks on the executor's
    self-pipe read end (parkForIo, ordinary fd interest — the VM poller
    harvests it). The worker fills the record under the table mutex;
    submitWork's ack byte (ackFd = self-pipe write end) then lands on
    the pipe — after fn returns, hence always after the fill. Record
    lifetime: the context releases on resume (including timeout), the
    worker marks done; the table frees only when both flags are set —
    a late completion never touches freed memory.

    Locking: same discipline as the slot table — every table access
    runs under NetExecState::m_mutex (plan ruling R9); the table takes
    no lock of its own.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nlang {

struct NetExecState;

//Job kinds.
constexpr int kNetJobResolve = 0;
constexpr int kNetJobIcmp = 1;

struct NetJobRecord {
    int kind = 0;
    int owner = 0;           //requesting context id (ping re-entry adoption)
    bool done = false;       //worker filled the result
    bool released = false;   //requesting context is done with it
    std::vector<std::string> endpoints;   //resolve: address texts
    int rttMs = 0;           //icmp (task 11)
    int err = 0;             //0 = ok, else the R10 code
};

//Free-list slot table, same shape as NetSocketTable. A record with a
//worker still in flight can never be freed (freeing needs done), so a
//recycled jobId never aliases a live worker's target — no generation.
class NetJobTable {
public:
    int32_t Mint(int kind, int owner = 0);   //jobId = index + 1
    //The record for a jobId, or null when free/stale.
    NetJobRecord* Lookup(int32_t jobId);
    //The jobId of this context's still-registered job of the kind
    //(_ping re-entry: the stack is rebuilt after the park unwind and
    //adopts the job it already submitted), or 0 when there is none.
    //A context runs one native at a time, so it can never own two
    //live jobs of one kind.
    int32_t FindOwn(int kind, int owner) const;
    //Fills the result and flags done (caller holds the state mutex;
    //the teardown-cv notify runs in NetFinishResolve after unlock).
    void MarkDone(int32_t jobId, std::vector<std::string>&& endpoints, int err);
    //ICMP variant: the result is the RTT instead of an endpoint list.
    void MarkDoneIcmp(int32_t jobId, int rttMs, int err);
    //Flags released and frees the record when done is also set.
    void Release(int32_t jobId);
    //Records still awaiting their worker (the dtor's cv predicate).
    int InFlightCount() const;

private:
    void FreeIfBoth(int32_t jobId);
    std::vector<NetJobRecord> m_records;
    std::vector<char> m_inUse;
    std::vector<int32_t> m_freeList;
};

//Fills the record under the state mutex, then notifies the teardown
//cv after the unlock (R9: the dtor waits for in-flight jobs).
void NetFinishResolve(NetExecState* st, int32_t jobId,
    std::vector<std::string>&& endpoints, int err);
//ICMP variant of NetFinishResolve (the pool worker's completion).
void NetFinishIcmp(NetExecState* st, int32_t jobId, int rttMs, int err);

//Drains the self-pipe read end until would-block (ack bytes are hints;
//the record under the mutex decides). Shared by the resolve wait
//(net_jobs.cpp) and the ping wait (net_icmp.cpp).
void NetDrainPipe(int readFd);

//The resolve natives behind net.n's connect path (registered by
//net_native.cpp; blocking-channel call shape).
void NetTcpResolveStart(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpResolveWait(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpResolveEndpoint(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpResolveRelease(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);

}  //namespace nlang
