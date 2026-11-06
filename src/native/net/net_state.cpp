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
        //precondition).
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
    //D12 backstop).
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_slots.CloseAllOpen();
    delete st;
}

void NetRememberLastError(NetExecState* st) {
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_lastErrorText = NetLastErrorText();
}

}  //namespace nlang
