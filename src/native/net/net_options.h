/*---
    net_options.h — the shared option-setter shape for the net natives
    (R10): resolve the handle's fd under the state mutex, apply one
    socket call, map a closed slot to -1 and a failure to -4 with the
    platform text recorded for _lastErrorText. The TCP and UDP option
    natives differ only in which socket call they apply, so they share
    these three arms (bool-, int- and string-valued). The bool travels
    as an int32 cell (the VM's bool representation).
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include <cstdint>
#include <string>

namespace nlang {

//Bool-valued option (broadcast, no-delay, keep-alive).
void NetApplyFlag(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, bool));

//Int-valued option (TTLs, buffer sizes).
void NetApplyInt(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, int));

//String-valued option (multicast group membership).
void NetApplyText(NativeHost* h, uint8_t* ret, const uint8_t* a,
    bool (*apply)(int, const std::string&));

}  //namespace nlang
