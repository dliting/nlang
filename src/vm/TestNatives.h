#pragma once
//Built-in test natives for ncc/nvm. These let the e2e suite exercise the
//native binding path (declaration → name lookup → call) deterministically;
//hosts embedding the VM register their own functions via
//VmExecutor::RegisterNative instead.
//
//ABI (nlang/vm/NativeHost.h): every native receives a NativeHost function
//table (for all VM access), the return cell, the argument cells and the
//declared parameter count. args[i] is the i-th uniform frame cell
//(kFrameSlotBytes = 8 bytes; a 4-byte argument's value is in the low
//half) — exactly the bytes the caller staged at callParamBase. The
//return value is memcpy'd into ret (may be null for void natives).
//
//Production note: ncc and nvm are test hosts — they always register these
//natives so the e2e suite can exercise the binding path. A production
//embedder would NOT include this header; scripts calling e.g.
//native_args.natAdd against a non-test host get
//"native function not registered".
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
    std::memcpy(&b, args + kFrameSlotBytes, sizeof(b));
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
    std::memcpy(&b, args + kFrameSlotBytes, sizeof(b));
    float r = a + b;
    std::memcpy(ret, &r, sizeof(r));
}

//void natPing() — no return value; ret must not be written.
inline void NatPing(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)host; (void)ret; (void)args; (void)argc;
}

//Phase 5: the VM keys of free `native` declarations are package-qualified
//("<package>.<name>"), and the package is the TU stem that declared them.
//This table is the measured (package x symbol) cross product of every
//e2e fixture's native declarations (methods would keep bare names —
//none here except the natConst method shape, registered bare for
//native_method.n).
inline void RegisterTestNatives(VmExecutor& executor) {
    executor.RegisterNative("native_basic.natConst", &NatConst);
    executor.RegisterNative("native_basic.natPing", &NatPing);
    executor.RegisterNative("native_args.natAdd", &NatAdd);
    executor.RegisterNative("native_default.natAdd", &NatAdd);
    executor.RegisterNative("native_float.natFAdd", &NatFAdd);
    executor.RegisterNative("nativelib.natConst", &NatConst);
    executor.RegisterNative("nativelib.natAdd", &NatAdd);
    executor.RegisterNative("func_ref_native.natConst", &NatConst);
    //native_method.n declares the native as a CLASS METHOD: methods keep
    //bare names (receiver dispatch), so its key is the bare natConst.
    executor.RegisterNative("natConst", &NatConst);
}

} // namespace nlang
