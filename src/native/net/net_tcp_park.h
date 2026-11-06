/*---
    net_tcp_park.h — the claim+park critical section shared by the TCP
    data-path natives: net_tcp.cpp (read/write/accept) and
    net_tcp_connect.cpp (connect). The park/retry loop itself lives in
    each native; this is the one piece of park bookkeeping that has to
    cross translation units (waiter discipline: net_tcp.cpp header).
---*/
#pragma once

#include "net_slots.h"
#include "net_state.h"

#include <nlang/vm/NativeHost.h>

#include <cstdint>

namespace nlang {

//Outcome of the claim+park critical section.
enum NetParkOutcome {
    kNetParkUnwound,     //first entry: interest registered, return now
    kNetParkReplay,      //re-entry after wake: retry the operation
    kNetParkClosed,      //slot gone — the closer won the window
    kNetParkConcurrent,  //a foreign context holds the direction (-3)
};

//Claim check, claim set and the parkForIo first-entry registration in
//one critical section (R9 lost-wakeup closure, net_tcp.cpp header
//note). Defined in net_tcp.cpp next to the waiters it manipulates.
NetParkOutcome NetClaimAndPark(NetExecState* st, int32_t handle, NetDir dir,
    int self, long long deadline, NativeHost* h);

}  //namespace nlang
