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

using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::NewArray;
using nlang::native::NewArrayShaped;
using nlang::native::NewArrayView;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//Element-count guard for arange: an absurd range with a tiny step must
//fail by name, not by attempting a multi-GB mint.
constexpr int64_t kArangeMaxElems = 100 * 1000 * 1000;

//full family: mint then fill with a constant.
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

//full(dims, value): the int[] dims vector shapes the mint, the fill
//value rides slot 1. The dims cells are collected through the shared
//rank-1 collector (stride-aware walk of a view's logical cells, bounded
//length) — a direct linear read would shape the result from the wrong
//cells whenever dims is a strided view. A negative dim raises the
//bounds family (sized creation); NewArrayShaped enforces the
//element-count cap.
template <typename T>
void FullDimsKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    uint8_t kind, T value) {
    NativeArrayView dims;
    ArgArray(host, args, 0, &dims);
    int32_t shape[nlang::kMaxArrayRank];
    const int32_t rank =
        CollectIntCells(host, dims, "tensor.full", shape);
    if (rank < 1)
        Raise(host, NEXC_Base, "tensor.full: dims length out of range");
    for (int32_t d = 0; d < rank; ++d) {
        if (shape[d] < 0)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.full: dims dim is negative");
    }
    const int32_t handle = NewArrayShaped(host, kind, rank, shape);
    NativeArrayView v;
    host->getArrayView(host, handle, &v);
    T* filled = CellsOf<T>(v);
    for (int64_t i = 0; i < v.elemCount; ++i)
        filled[i] = value;
    ReturnInt(ret, handle);
}

//Half-open arithmetic progression start, start+step, ... strictly before
//stop. The element count is ceil(|span/step|) — exact for integral steps
//and the standard numpy semantics for fractional steps. long double keeps
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

//Dims-shaped twin: same shape, but the mint is FullDimsKernel (the
//dims vector is slot 0, the fill value slot 1).
#define TENSOR_CONST_FILL_DIMS_BY_ARG(Name, Kind, T, Reader)           \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,     \
              int argc) {                                              \
        (void)argc;                                                    \
        FullDimsKernel<T>(host, ret, args, (Kind),                     \
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

TENSOR_CONST_FILL_BY_ARG(FullI, NEK_Int32, int32_t, ReadI)
TENSOR_CONST_FILL_BY_ARG(FullL, NEK_Int64, int64_t, ReadL)
TENSOR_CONST_FILL_BY_ARG(FullF, NEK_Float32, float, ReadF)
TENSOR_CONST_FILL_BY_ARG(FullD, NEK_Float64, double, ReadD)
TENSOR_CONST_FILL_DIMS_BY_ARG(FullDimsI, NEK_Int32, int32_t, ReadI)
TENSOR_CONST_FILL_DIMS_BY_ARG(FullDimsL, NEK_Int64, int64_t, ReadL)
TENSOR_CONST_FILL_DIMS_BY_ARG(FullDimsF, NEK_Float32, float, ReadF)
TENSOR_CONST_FILL_DIMS_BY_ARG(FullDimsD, NEK_Float64, double, ReadD)
TENSOR_ARANGE(ArangeI, NEK_Int32, int32_t, ReadI)
TENSOR_ARANGE(ArangeL, NEK_Int64, int64_t, ReadL)
TENSOR_ARANGE(ArangeF, NEK_Float32, float, ReadF)
TENSOR_ARANGE(ArangeD, NEK_Float64, double, ReadD)

//ABI v10 exercise pair. strideViewI mints a rank-1 view through the
//new newArrayView host entry (len/stride/offset in element units —
//the validation raises exactly like the language's view creation);
//viewSumI walks any rank-1 int array or view through getArrayView,
//addressing elements as data[(offset + i*strides[0]) * elemCellBytes]
//(owning arrays report offset 0 and row-major strides).
void StrideViewI(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 int argc) {
    (void)argc;
    const int32_t target = ArgInt(args, 0);
    const int32_t len = ArgInt(args, 1);
    const int32_t stride = ArgInt(args, 2);
    const int32_t offset = ArgInt(args, 3);
    const int32_t dims[1] = { len };
    const int32_t strides[1] = { stride };
    ReturnInt(ret, NewArrayView(host, target, 1, dims, strides, offset));
}

void ViewSumI(NativeHost* host, uint8_t* ret, const uint8_t* args,
              int argc) {
    (void)argc;
    NativeArrayView view;
    ArgArray(host, args, 0, &view);
    int64_t sum = 0;
    const int32_t* payload = static_cast<const int32_t*>(view.data);
    for (int64_t i = 0; i < view.elemCount; ++i)
        sum += payload[view.offset + i * view.strides[0]];
    ReturnInt(ret, static_cast<int32_t>(sum));
}

} // namespace tensor

NLANG_DEFINE_NATIVE_INIT {
    regBlocking(registry, "tensor", "fullI", &tensor::FullI);
    regBlocking(registry, "tensor", "fullL", &tensor::FullL);
    regBlocking(registry, "tensor", "fullF", &tensor::FullF);
    regBlocking(registry, "tensor", "fullD", &tensor::FullD);
    regBlocking(registry, "tensor", "fullDimsI", &tensor::FullDimsI);
    regBlocking(registry, "tensor", "fullDimsL", &tensor::FullDimsL);
    regBlocking(registry, "tensor", "fullDimsF", &tensor::FullDimsF);
    regBlocking(registry, "tensor", "fullDimsD", &tensor::FullDimsD);
    regBlocking(registry, "tensor", "arangeI", &tensor::ArangeI);
    regBlocking(registry, "tensor", "arangeL", &tensor::ArangeL);
    regBlocking(registry, "tensor", "arangeF", &tensor::ArangeF);
    regBlocking(registry, "tensor", "arangeD", &tensor::ArangeD);
    regBlocking(registry, "tensor", "strideViewI", &tensor::StrideViewI);
    regBlocking(registry, "tensor", "viewSumI", &tensor::ViewSumI);
    regBlocking(registry, "tensor", "add", &tensor::TensorAdd);
    regBlocking(registry, "tensor", "sub", &tensor::TensorSub);
    regBlocking(registry, "tensor", "mul", &tensor::TensorMul);
    regBlocking(registry, "tensor", "div", &tensor::TensorDiv);
    regBlocking(registry, "tensor", "exp", &tensor::TensorExp);
    regBlocking(registry, "tensor", "log", &tensor::TensorLog);
    regBlocking(registry, "tensor", "sum", &tensor::TensorSum);
    regBlocking(registry, "tensor", "max", &tensor::TensorMax);
    regBlocking(registry, "tensor", "min", &tensor::TensorMin);
    regBlocking(registry, "tensor", "mean", &tensor::TensorMean);
    regBlocking(registry, "tensor", "dot", &tensor::TensorDot);
    regBlocking(registry, "tensor", "transpose", &tensor::TensorTranspose);
    regBlocking(registry, "tensor", "flatten", &tensor::TensorFlatten);
    //One reshape entry: the target shape rides the int[] argument.
    regBlocking(registry, "tensor", "reshape", &tensor::TensorReshape);
    regBlocking(registry, "tensor", "concat", &tensor::TensorConcat);
    regBlocking(registry, "tensor", "matmul", &tensor::TensorMatmul);
    //v0.12.7 Task 12: the view family and the scalar arms.
    regBlocking(registry, "tensor", "slice", &tensor::TensorSlice);
    regBlocking(registry, "tensor", "broadcastTo",
                &tensor::TensorBroadcastTo);
    regBlocking(registry, "tensor", "addScalar",
                &tensor::TensorAddScalar);
    regBlocking(registry, "tensor", "subScalar",
                &tensor::TensorSubScalar);
    regBlocking(registry, "tensor", "mulScalar",
                &tensor::TensorMulScalar);
    regBlocking(registry, "tensor", "divScalar",
                &tensor::TensorDivScalar);
    return NLANG_HOST_ABI_VERSION;
}
