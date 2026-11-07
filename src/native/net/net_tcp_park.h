/*---
    net_tcp_park.h — the claim+park critical section and the shared
    claim/release/park helpers used by every parking native: the TCP
    TUs (net_tcp.cpp read/write/accept, net_tcp_connect.cpp connect)
    and the UDP receive arms (net_udp.cpp). The park/retry loop itself
    lives in each native; these are the pieces of park bookkeeping and
    byte-array marshalling that have to cross translation units
    (waiter discipline: net_tcp.cpp header). Definitions live in
    net_tcp.cpp next to the waiters they manipulate.
---*/
#pragma once

#include "net_slots.h"
#include "net_state.h"

#include <nlang/vm/NativeHost.h>

#include <cstdint>
#include <vector>

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

//Releases the direction's claim if this call holds it (R9 mutex, R5
//deferred close).
void NetRelease(NetExecState* st, int32_t handle, NetDir dir, bool claimed);

//Outcome of the claim-adopt critical section.
enum NetAdoptOutcome {
    kNetAdoptOk,          //slot live; *claimed = this call holds the claim
    kNetAdoptGone,        //slot gone — the closer won the window
    kNetAdoptConcurrent,  //a foreign context holds the direction (-3)
};

//Reads the slot's fd and adopts this call's claim under the mutex.
//kNetAdoptGone = the slot is gone (the direction's waiter is cleared);
//kNetAdoptConcurrent = a foreign waiter owns the direction, so the
//caller must not touch its per-direction state (the claim is left as
//the foreign call set it — this call never claimed). On Ok,
//*freshCall (when given) tells a first entry (no prior waiter) from
//a park re-entry (waiter == self): the write path resets its
//writeSent progress stash only on a fresh entry, so a call after a
//timed-out or failed write restarts the payload at zero (0.12.9
//review I-1 — the stash must survive parks within one call and die
//with it).
NetAdoptOutcome NetAdoptClaim(NetExecState* st, int32_t handle, NetDir dir,
    int self, int* fd, bool* claimed, bool* freshCall = nullptr);

//Maps a rejected adopt to its R10 error code (Gone -> closed,
//Concurrent -> concurrent).
inline int NetAdoptErrCode(NetAdoptOutcome adopt) {
    return adopt == kNetAdoptGone ? kNetErrClosed : kNetErrConcurrent;
}

//Shared park tail: true = the caller must return (unwound, or closed/
//concurrent with ret staged). False = replay the operation.
bool NetParkTail(NetExecState* st, int32_t handle, NetDir dir,
    NetParkOutcome outcome, bool claimed, uint8_t* ret);

//Byte-array narrow-cell marshalling (array store convention: one
//int32 cell per element, low byte = value).
std::vector<uint8_t> NetPackBytes(const NativeArrayView& view, int count);
void NetUnpackBytes(const NativeArrayView& view, const uint8_t* bytes,
    int count);

}  //namespace nlang
