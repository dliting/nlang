/*---
    net_state.cpp — the NetExecState ctor/dtor pair behind the ABI
    moduleState slot (net_state.h).
---*/
#include "net_state.h"

#include "net_platform.h"

namespace nlang {

void* NetStateCtor() { return new NetExecState(); }

void NetStateDtor(void* state) {
    if (!state)
        return;
    auto* st = static_cast<NetExecState*>(state);
    //Executor teardown closes sockets the script never closed (design
    //D12 backstop). From task 7 this also waits for in-flight jobs
    //before freeing (R9).
    std::lock_guard<std::mutex> g(st->m_mutex);
    st->m_slots.CloseAllOpen();
    delete st;
}

}  //namespace nlang
