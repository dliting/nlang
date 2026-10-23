/*---
tensor native module — the dynamically-loaded implementation of the
`tensor` package's numeric kernels. Built as nlang_tensor.dll /
libnlang_tensor.so; the VM loads it through the same mechanism as any
third-party native library.

This module depends only on the stable NativeHost ABI and the C++
standard library — it does not link nlang_vm. Kernels receive pinned
payload views (getArrayView), compute on them directly, and mint results
through newArray; see include/nlang/vm/NativeHost.h for the pointer-
validity contract.

All kernels register through the blocking channel: array work is
unbounded in element count, so the global lock must be released for the
duration (the staged-argument roots keep the operands alive).
---*/
#include <nlang/vm/NativeHost.h>

#include <cmath>
#include <cstdint>

using nlang::native::ArgInt;
using nlang::native::NewArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace {

//double[] zerosD(int n) — zero-initialized minting is the host's job, so
//the kernel only needs to hand the minted handle back.
void ZerosD(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    const int32_t n = ArgInt(args, 0);
    const int32_t handle = NewArray(host, NEK_Float64, n);
    ReturnInt(ret, handle);
}

} // namespace

NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "tensor", "zerosD", &ZerosD);
    return NLANG_HOST_ABI_VERSION;
}
