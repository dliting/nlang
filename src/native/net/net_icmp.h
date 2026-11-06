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

}  //namespace nlang
