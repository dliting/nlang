/*---
    net_udp.h — the UDP natives' registration hook: the natives
    themselves live in net_udp.cpp, and net_native.cpp's module init
    forwards to NetRegisterUdpNatives (keeping the 18 datagram
    registrations out of the TCP-centric entry TU).
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

namespace nlang {

void NetRegisterUdpNatives(void* registry, RegisterNativeFn reg,
    RegisterNativeBlockingFn regBlocking);

}  //namespace nlang
