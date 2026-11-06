/*---
    net_state.h — per-executor module state, host-managed through ABI
    v11 moduleState (design §6, plan ruling R8). Holds the socket slot
    table, the per-context last-error text, the job table with its
    self-pipe and
    the in-flight connect stash (task 7). The dtor closes every
    still-open fd (executor teardown = design D12 backstop) and waits
    for in-flight jobs before freeing (R9). m_mutex guards every other
    member: r1 blocking natives run with the GIL released, so the
    committed "all calls under the GIL" table invariant no longer
    holds (plan ruling R9). Lock order: m_mutex -> GIL (parkForIo/
    wakeIoWaiters take the GIL internally). Discipline (external
    review I7): no potentially blocking syscall runs under m_mutex —
    snapshot the inputs, run the syscall outside the lock, revalidate
    the generation-coded handle on re-acquisition; the µs-fast state
    queries (getsockname/getpeername arms) are the accepted exception.
---*/
#pragma once

#include "net_jobs.h"
#include "net_slots.h"

#include <nlang/vm/NativeHost.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nlang {

//R10 error-code table: net natives return these as their int result
//(no raiseException — the script's _throwNet maps them to
//SocketException/TimeoutException; -4 paths record the platform text
//in the per-context m_lastErrorText for _lastErrorText).
constexpr int32_t kNetErrClosed = -1;
constexpr int32_t kNetErrTimeout = -2;
constexpr int32_t kNetErrConcurrent = -3;
constexpr int32_t kNetErrPlatform = -4;
constexpr int32_t kNetErrArg = -5;

struct NetExecState {
    std::mutex m_mutex;
    NetSocketTable m_slots;
    //Last platform error text, keyed by the requesting context (the
    //R14 identity — external review M10): the failing native records
    //it, the same context's _throwNet consumes it through
    //_lastErrorText (erased on read), and a co-context's failure can
    //never surface as this context's text. Context ids are monotonic
    //per scheduler, so an entry never aliases a different context;
    //erase-on-read keeps the map bounded to failures still pending
    //their read.
    std::unordered_map<int, std::string> m_lastErrorText;
    //Job offload (net_jobs.h): the table plus the self-pipe the
    //requesting context parks on — [0] read end (park target), [1]
    //write end (submitWork's ackFd). Built in the ctor through
    //NetSocketPair; -1 pair on failure (the wait path reports -4).
    NetJobTable m_jobs;
    int m_selfPipeFds[2] = {-1, -1};
    bool m_jobsShutdown = false;   //dtor: drain in-flight jobs (R9)
    std::condition_variable m_jobsDoneCv;
    //In-flight non-blocking connects: re-entry restarts the native
    //from its first line, so the half-open stream's handle is stashed
    //per context (one call at a time per context — R14 identity).
    std::unordered_map<int, int32_t> m_connectInFlight;
};

//Ctor/dtor behind the moduleState slot (the C ABI callback shapes).
void* NetStateCtor();
void NetStateDtor(void* state);

//Records the current OS error text for _lastErrorText under the
//calling context's id (R10 -4 paths, M10). Takes the state mutex —
//call only while it is NOT already held.
void NetRememberLastError(NativeHost* h);

//The executor's NetExecState, created on the first call. The tag
//carries the layout version — bump it whenever NetExecState changes
//(v3: the last-error text became a per-context map, M10).
inline NetExecState* NetState(NativeHost* host) {
    return static_cast<NetExecState*>(host->moduleState(
        host, "nlang.net.v3", &NetStateCtor, &NetStateDtor));
}

}  //namespace nlang
