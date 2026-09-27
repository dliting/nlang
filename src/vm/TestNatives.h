#pragma once
//Built-in test natives for ncc/nvm. These let the e2e suite exercise the
//native binding path (declaration → name lookup → call) deterministically;
//hosts embedding the VM register their own functions via
//VmExecutor::RegisterNative instead.
//
//ABI (nlang/vm/NativeHost.h): every native receives a NativeHost function
//table (for all VM access), the return cell, the argument cells and the
//declared parameter count. args[i] is the i-th 4-byte cell — raw int32 /
//float bits or a heap handle. The return value is written into ret (left
//untouched for void natives).
//
//Production note: ncc and nvm are test hosts — they always register these
//natives so the e2e suite can exercise the binding path. A production
//embedder would NOT include this header; scripts calling natAdd/... against
//a non-test host get "native function not registered".
#include "VmExecutor.h"
#include "nlang/vm/NativeHost.h"
#include <cstdint>
#include <cstring>

namespace nlang {

//int natAdd(int a, int b) — returns a + b.
inline void NatAdd(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)host; (void)argc;
    int32_t a, b;
    std::memcpy(&a, args, sizeof(a));
    std::memcpy(&b, args + NLANG_VALUE_SIZE, sizeof(b));
    int32_t r = a + b;
    std::memcpy(ret, &r, sizeof(r));
}

//int natConst() — returns 77 (zero-arg path).
inline void NatConst(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc) {
    (void)host; (void)args; (void)argc;
    int32_t r = 77;
    std::memcpy(ret, &r, sizeof(r));
}

//float natFAdd(float a, float b) — returns a + b (float ABI check).
inline void NatFAdd(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)host; (void)argc;
    float a, b;
    std::memcpy(&a, args, sizeof(a));
    std::memcpy(&b, args + NLANG_VALUE_SIZE, sizeof(b));
    float r = a + b;
    std::memcpy(ret, &r, sizeof(r));
}

//void natPing() — no return value; ret must not be written.
inline void NatPing(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)host; (void)ret; (void)args; (void)argc;
}

inline void RegisterTestNatives(VmExecutor& executor) {
    executor.RegisterNative("natAdd", &NatAdd);
    executor.RegisterNative("natConst", &NatConst);
    executor.RegisterNative("natFAdd", &NatFAdd);
    executor.RegisterNative("natPing", &NatPing);
}

} // namespace nlang
