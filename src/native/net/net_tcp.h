/*---
    net_tcp.h — the TCP data-path native entry points (connect
    implemented in net_tcp_connect.cpp, the rest in net_tcp.cpp),
    registered by net_native.cpp (blocking-channel call shape from
    NativeHost.h).
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include <cstdint>

namespace nlang {

void NetTcpAccept(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpConnectTry(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpRead(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpWrite(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpShutdownWrite(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpIsClosed(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpPeerAddress(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);
void NetTcpPeerPort(NativeHost* h, uint8_t* ret, const uint8_t* a, int argc);

}  //namespace nlang
