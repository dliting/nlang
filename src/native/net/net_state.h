/*---
    net_state.h — per-executor module state, host-managed through ABI
    v11 moduleState (design §6, plan ruling R8). Holds the socket slot
    table and the last-error text; the job table and self-pipe join in
    task 7. The dtor closes every still-open fd (executor teardown =
    design D12 backstop); from task 7 it also waits for in-flight jobs
    before freeing (R9). m_mutex guards m_slots and m_lastErrorText:
    r1 blocking natives run with the GIL released, so the committed
    "all calls under the GIL" table invariant no longer holds (plan
    ruling R9). Lock order: m_mutex -> GIL (parkForIo/wakeIoWaiters
    take the GIL internally).
---*/
#pragma once

#include "net_slots.h"

#include <nlang/vm/NativeHost.h>

#include <mutex>
#include <string>

namespace nlang {

//R10 error-code table: net natives return these as their int result
//(no raiseException — the script's _throwNet maps them to
//SocketException/TimeoutException; -4 paths record the platform text
//in m_lastErrorText for _lastErrorText).
constexpr int32_t kNetErrClosed = -1;
constexpr int32_t kNetErrTimeout = -2;
constexpr int32_t kNetErrConcurrent = -3;
constexpr int32_t kNetErrPlatform = -4;
constexpr int32_t kNetErrArg = -5;

struct NetExecState {
    std::mutex m_mutex;
    NetSocketTable m_slots;
    std::string m_lastErrorText;
};

//Ctor/dtor behind the moduleState slot (the C ABI callback shapes).
void* NetStateCtor();
void NetStateDtor(void* state);

//The executor's NetExecState, created on the first call. The tag
//carries the layout version — bump it whenever NetExecState changes.
inline NetExecState* NetState(NativeHost* host) {
    return static_cast<NetExecState*>(host->moduleState(
        host, "nlang.net.v1", &NetStateCtor, &NetStateDtor));
}

}  //namespace nlang
