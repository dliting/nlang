/*---
    net_icmp.h — the ICMP natives' registration hook: the natives
    themselves live in net_icmp.cpp, and net_native.cpp's module init
    forwards to NetRegisterIcmpNatives (same shape as net_udp.h).
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

namespace nlang {

void NetRegisterIcmpNatives(void* registry, RegisterNativeFn reg,
    RegisterNativeBlockingFn regBlocking);

//Echo slice (external review I3): the helper timeout is always bounded
//by this, so one ping job never holds a pool worker longer than one
//slice — teardown and the priority-0 DNS queue are never hostage to a
//black-hole host. A slice that expires while the script deadline still
//allows re-arms with a fresh echo (retransmission, the shape every
//ping tool uses).
constexpr int kIcmpEchoSliceMs = 5000;

//Echo timeout for the helper: the time left until the deadline capped
//by the echo slice; deadline 0 (unlimited) gets one slice. Pure
//(unit-tested in test_net_icmp.cpp).
int IcmpEchoTimeout(long long deadline, long long nowMs);

//Queue priority for one echo job: 1 (the latency-sensitive lane) when the
//caller set a deadline it has to meet, 0 for the unbounded wait, which has
//no latency of its own to protect (0.13.6 perf spec D6).
int IcmpJobPriority(long long deadline);

//Whether a finished job with this err must be re-submitted: only a
//slice timeout while the deadline still allows (deadline 0 always
//allows). Reply and unreachable verdicts are terminal.
bool IcmpShouldResubmit(int err, long long deadline, long long nowMs);

}  //namespace nlang
