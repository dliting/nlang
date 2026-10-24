/*---
tensor_native.cpp — registration unit and the creation-kernel family of
the `tensor` package's native module (nlang_tensor.dll). The elementwise
binary family lives in tensor_binary.cpp; every entry point is declared
in TensorKernels.h. See that header and include/nlang/vm/NativeHost.h
for the payload-view contract.

Depends only on the stable NativeHost ABI and the C++ standard library —
it does not link nlang_vm. All kernels register through the blocking
channel: array work is unbounded in element count, so the global lock
must be released for the duration (the staged-argument roots keep the
operands alive).
---*/
#include "TensorKernels.h"

#include <cmath>
#include <cstdint>

using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::NewArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//Element-count guard for arange: an absurd range with a tiny step must
//fail by name, not by attempting a multi-GB mint.
constexpr int64_t kArangeMaxElems = 100 * 1000 * 1000;

//zeros/ones/full family: mint then fill with a constant.
template <typename T>
void FillKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                uint8_t kind, T value) {
    const int32_t n = ArgInt(args, 0);
    const int32_t handle = NewArray(host, kind, n);
    NativeArrayView v;
    host->getArrayView(host, handle, &v);
    T* cells = CellsOf<T>(v);
    for (int64_t i = 0; i < v.elemCount; ++i)
        cells[i] = value;
    ReturnInt(ret, handle);
}

//Half-open arithmetic progression start, start+step, ... strictly before
//stop. The element count is ceil(|span/step|) — exact for integral steps
//and the standard numpy semantics for fractional ones. long double keeps
//float/double span division from pre-rounding the ceiling.
template <typename T>
void ArangeKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  uint8_t kind, T start, T stop, T step) {
    if (step == T(0) || !((stop > start) == (step > 0)))
        Raise(host, NEXC_Base,
              "tensor.arange: step must advance toward stop");
    const long double span = static_cast<long double>(stop)
        - static_cast<long double>(start);
    const long double advance = static_cast<long double>(step);
    const long double ratio = std::fabsl(span / advance);
    if (ratio > static_cast<long double>(kArangeMaxElems))
        Raise(host, NEXC_Base, "tensor.arange: range too large");
    const int64_t n = static_cast<int64_t>(std::ceill(ratio));
    const int32_t handle = NewArray(host, kind, static_cast<int32_t>(n));
    NativeArrayView v;
    host->getArrayView(host, handle, &v);
    T* cells = CellsOf<T>(v);
    T cur = start;
    for (int64_t i = 0; i < v.elemCount; ++i) {
        cells[i] = cur;
        cur = static_cast<T>(cur + step);
    }
    ReturnInt(ret, handle);
}

} // namespace

#define TENSOR_CONST_FILL(Name, Kind, T, Value)                        \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,     \
              int argc) {                                              \
        (void)argc;                                                    \
        FillKernel<T>(host, ret, args, (Kind), (Value));               \
    }

#define TENSOR_CONST_FILL_BY_ARG(Name, Kind, T, Reader)                \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,     \
              int argc) {                                              \
        (void)argc;                                                    \
        FillKernel<T>(host, ret, args, (Kind),                         \
            static_cast<T>((Reader)(args, 1)));                        \
    }

#define TENSOR_ARANGE(Name, Kind, T, Reader)                           \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,     \
              int argc) {                                              \
        (void)argc;                                                    \
        ArangeKernel<T>(host, ret, args, (Kind),                       \
            static_cast<T>((Reader)(args, 0)),                         \
            static_cast<T>((Reader)(args, 1)),                         \
            static_cast<T>((Reader)(args, 2)));                        \
    }

namespace {
int32_t ReadI(const uint8_t* args, int slot) { return ArgInt(args, slot); }
int64_t ReadL(const uint8_t* args, int slot) { return ArgLong(args, slot); }
float   ReadF(const uint8_t* args, int slot) { return ArgFloat(args, slot); }
double  ReadD(const uint8_t* args, int slot) { return ArgDouble(args, slot); }
} // namespace

TENSOR_CONST_FILL(ZerosI, NEK_Int32, int32_t, 0)
TENSOR_CONST_FILL(ZerosL, NEK_Int64, int64_t, 0)
TENSOR_CONST_FILL(ZerosF, NEK_Float32, float, 0.0f)
TENSOR_CONST_FILL(ZerosD, NEK_Float64, double, 0.0)
TENSOR_CONST_FILL(OnesI, NEK_Int32, int32_t, 1)
TENSOR_CONST_FILL(OnesL, NEK_Int64, int64_t, 1)
TENSOR_CONST_FILL(OnesF, NEK_Float32, float, 1.0f)
TENSOR_CONST_FILL(OnesD, NEK_Float64, double, 1.0)
TENSOR_CONST_FILL_BY_ARG(FullI, NEK_Int32, int32_t, ReadI)
TENSOR_CONST_FILL_BY_ARG(FullL, NEK_Int64, int64_t, ReadL)
TENSOR_CONST_FILL_BY_ARG(FullF, NEK_Float32, float, ReadF)
TENSOR_CONST_FILL_BY_ARG(FullD, NEK_Float64, double, ReadD)
TENSOR_ARANGE(ArangeI, NEK_Int32, int32_t, ReadI)
TENSOR_ARANGE(ArangeL, NEK_Int64, int64_t, ReadL)
TENSOR_ARANGE(ArangeF, NEK_Float32, float, ReadF)
TENSOR_ARANGE(ArangeD, NEK_Float64, double, ReadD)

} // namespace tensor

NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "tensor", "zerosI", &tensor::ZerosI);
    regBlocking(registry, "tensor", "zerosL", &tensor::ZerosL);
    regBlocking(registry, "tensor", "zerosF", &tensor::ZerosF);
    regBlocking(registry, "tensor", "zerosD", &tensor::ZerosD);
    regBlocking(registry, "tensor", "onesI", &tensor::OnesI);
    regBlocking(registry, "tensor", "onesL", &tensor::OnesL);
    regBlocking(registry, "tensor", "onesF", &tensor::OnesF);
    regBlocking(registry, "tensor", "onesD", &tensor::OnesD);
    regBlocking(registry, "tensor", "fullI", &tensor::FullI);
    regBlocking(registry, "tensor", "fullL", &tensor::FullL);
    regBlocking(registry, "tensor", "fullF", &tensor::FullF);
    regBlocking(registry, "tensor", "fullD", &tensor::FullD);
    regBlocking(registry, "tensor", "arangeI", &tensor::ArangeI);
    regBlocking(registry, "tensor", "arangeL", &tensor::ArangeL);
    regBlocking(registry, "tensor", "arangeF", &tensor::ArangeF);
    regBlocking(registry, "tensor", "arangeD", &tensor::ArangeD);
    regBlocking(registry, "tensor", "add", &tensor::TensorAdd);
    regBlocking(registry, "tensor", "sub", &tensor::TensorSub);
    regBlocking(registry, "tensor", "mul", &tensor::TensorMul);
    regBlocking(registry, "tensor", "div", &tensor::TensorDiv);
    return NLANG_HOST_ABI_VERSION;
}
