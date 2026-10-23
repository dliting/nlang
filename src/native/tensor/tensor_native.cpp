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

using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::NewArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace {

//Element-count guard for arange: an absurd range with a tiny step must
//fail by name, not by attempting a multi-GB mint.
constexpr int64_t kArangeMaxElems = 100 * 1000 * 1000;

template <typename T>
T* CellsOf(const NativeArrayView& v) {
    return static_cast<T*>(v.data);
}

//--- creation kernels ----------------------------------------------------------

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

//--- per-kind argument readers --------------------------------------------------

int32_t ReadI(const uint8_t* args, int slot) { return ArgInt(args, slot); }
int64_t ReadL(const uint8_t* args, int slot) { return ArgLong(args, slot); }
float   ReadF(const uint8_t* args, int slot) { return ArgFloat(args, slot); }
double  ReadD(const uint8_t* args, int slot) { return ArgDouble(args, slot); }

//--- registered entry points -----------------------------------------------------
//Each macro instantiates one blocking native over a shared template; the
//registration list at the bottom is the package's public name set.

#define TENSOR_CONST_FILL(Name, Kind, T, Value)                             \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {   \
        FillKernel<T>(host, ret, args, (Kind), (Value));                    \
    }

#define TENSOR_CONST_FILL_BY_ARG(Name, Kind, T, Reader)                     \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {   \
        FillKernel<T>(host, ret, args, (Kind),                             \
            static_cast<T>((Reader)(args, 1)));                             \
    }

#define TENSOR_ARANGE(Name, Kind, T, Reader)                                \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {   \
        ArangeKernel<T>(host, ret, args, (Kind),                            \
            static_cast<T>((Reader)(args, 0)),                              \
            static_cast<T>((Reader)(args, 1)),                              \
            static_cast<T>((Reader)(args, 2)));                             \
    }

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

} // namespace

NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "tensor", "zerosI", &ZerosI);
    regBlocking(registry, "tensor", "zerosL", &ZerosL);
    regBlocking(registry, "tensor", "zerosF", &ZerosF);
    regBlocking(registry, "tensor", "zerosD", &ZerosD);
    regBlocking(registry, "tensor", "onesI", &OnesI);
    regBlocking(registry, "tensor", "onesL", &OnesL);
    regBlocking(registry, "tensor", "onesF", &OnesF);
    regBlocking(registry, "tensor", "onesD", &OnesD);
    regBlocking(registry, "tensor", "fullI", &FullI);
    regBlocking(registry, "tensor", "fullL", &FullL);
    regBlocking(registry, "tensor", "fullF", &FullF);
    regBlocking(registry, "tensor", "fullD", &FullD);
    regBlocking(registry, "tensor", "arangeI", &ArangeI);
    regBlocking(registry, "tensor", "arangeL", &ArangeL);
    regBlocking(registry, "tensor", "arangeF", &ArangeF);
    regBlocking(registry, "tensor", "arangeD", &ArangeD);
    return NLANG_HOST_ABI_VERSION;
}
