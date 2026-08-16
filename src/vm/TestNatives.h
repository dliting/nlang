#pragma once
//Phase 9f: built-in test natives for ncc/nvm. These exist so the e2e
//suite can exercise the native binding path (declaration → table lookup
//→ call) deterministically; hosts embedding the VM register their own
//table via VmExecutor::RegisterNative instead.
//
//ABI reminder (VmExecutor.h): args[i] is the i-th 4-byte argument cell —
//raw int32/float bits or heap idx, exactly the bytes the caller staged at
//callParamBase. The return value is memcpy'd into ret (may be null for
//void natives). argc is the declaration's paramCount.
//
//Production note: ncc and nvm are test hosts — they always register these
//natives so the e2e suite can exercise the binding path. A production
//embedder would NOT include this header; they register their own table
//via VmExecutor::RegisterNative. Scripts calling natAdd/natConst/etc.
//against a non-test host will get "native function not registered".
#include "VmExecutor.h"
#include <cstdint>
#include <cstring>

namespace nlang {

//int natAdd(int a, int b) — returns a + b.
inline void NatAdd(uint8_t* ret, const uint8_t* args, int argc) {
    (void)argc;
    int32_t a, b;
    std::memcpy(&a, args, sizeof(a));
    std::memcpy(&b, args + 4, sizeof(b));
    int32_t r = a + b;
    std::memcpy(ret, &r, sizeof(r));
}

//int natConst() — returns 77 (zero-arg path).
inline void NatConst(uint8_t* ret, const uint8_t* args, int argc) {
    (void)args; (void)argc;
    int32_t r = 77;
    std::memcpy(ret, &r, sizeof(r));
}

//float natFAdd(float a, float b) — returns a + b (float ABI check).
inline void NatFAdd(uint8_t* ret, const uint8_t* args, int argc) {
    (void)argc;
    float a, b;
    std::memcpy(&a, args, sizeof(a));
    std::memcpy(&b, args + 4, sizeof(b));
    float r = a + b;
    std::memcpy(ret, &r, sizeof(r));
}

//void natPing() — no return value; ret must not be written.
inline void NatPing(uint8_t* ret, const uint8_t* args, int argc) {
    (void)ret; (void)args; (void)argc;
}

inline void RegisterTestNatives(VmExecutor& executor) {
    executor.RegisterNative("natAdd", &NatAdd);
    executor.RegisterNative("natConst", &NatConst);
    executor.RegisterNative("natFAdd", &NatFAdd);
    executor.RegisterNative("natPing", &NatPing);
}

} // namespace nlang
