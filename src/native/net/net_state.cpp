/*---
    net_state.cpp — the NetExecState ctor/dtor pair behind the ABI
    moduleState slot (net_state.h).
---*/
#include "net_state.h"

#include "net_platform.h"

#include <mutex>

namespace nlang {

void* NetStateCtor() {
    //Winsock up before the self-pair (R4 once flag).
    NetPlatformInit();
    auto* st = new NetExecState();
    //The job ack channel (net_jobs.h). On failure the fds stay -1 and
    //the resolve wait reports -4 instead of parking on a dead fd.
    NetSocketPair(st->m_selfPipeFds);
    return st;
}

void NetStateDtor(void* state) {
    if (!state)
        return;
    auto* st = static_cast<NetExecState*>(state);
    {
        //R9: pool workers write job records through the state pointer;
        //the free must wait for every in-flight job to fill (the pool
        //itself is joined later, at scheduler teardown — R8
        //precondition). The wait is bounded, not open-ended: every
        //helper timeout is capped (the ICMP echo slice, net_icmp.h;
        //getaddrinfo's own resolver timeout), so teardown is never
        //hostage to external network state (external review I3).
        std::unique_lock<std::mutex> g(st->m_mutex);
        st->m_jobsShutdown = true;
        st->m_jobsDoneCv.wait(g, [st] {
            return st->m_jobsShutdown && st->m_jobs.InFlightCount() == 0;
        });
    }
    //Benign race (review round 3, Info): the pool's ack byte may still
    //be in flight when these close — send-to-closed errors are ignored
    //and no waiter is parked during teardown, so the level-triggered
    //re-check contract covers the Windows handle-reuse byte window.
    if (st->m_selfPipeFds[0] >= 0)
        NetClose(st->m_selfPipeFds[0]);
    if (st->m_selfPipeFds[1] >= 0)
        NetClose(st->m_selfPipeFds[1]);
    //Executor teardown closes sockets the script never closed (design
    //D12 backstop). Invariant (external review M14): a module may
    //close fds the VM poller still watches only in this window —
    //the normal close arms deregister through wakeIoWaiters first,
    //but the moduleState dtor has no host to call, so the poller
    //keeps stale registrations (these fds and the self-pipe read
    //end above) until the scheduler destructor joins it. Benign by
    //construction: the run has ended (carriers joined, no context
    //parked) and no new fd is minted here, so HUP/ERR events the
    //poller reports for a closed fd wake nobody.
    //The delete sits OUTSIDE the guard's scope: destroying the state
    //destroys m_mutex, and ~mutex on a still-owned mutex is undefined
    //(the Debug STL check fires "mutex destroyed while busy"; the
    //guard's unlock then reads freed storage). Close under the lock,
    //release, then free — the last reference is this dtor, so nothing
    //races the gap.
    {
        std::lock_guard<std::mutex> g(st->m_mutex);
        st->m_slots.CloseAllOpen();
    }
    delete st;
}

void NetRememberLastError(NativeHost* h) {
    NetExecState* st = NetState(h);
    std::lock_guard<std::mutex> g(st->m_mutex);
    std::string text;
    int code = 0;
    NetLastErrorInfo(text, code);   //one read: text and code share the error
    st->m_lastErrorText[h->currentContextId(h)] = text;
    st->m_lastErrorCode[h->currentContextId(h)] = code;
}

}  //namespace nlang
