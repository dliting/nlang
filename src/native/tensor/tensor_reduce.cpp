/*---
tensor_reduce.cpp — the reduction family (sum/max/min/mean/dot) of the
tensor native module. Same dispatch shape as the binary family: one
registered native per operation name, the first view's runtime element
kind picks the typed kernel. Integer sums wrap like language int
arithmetic (declared T return); dot accumulates in 64 bits and returns
long so large inner products never wrap; mean always returns double.
---*/
#include "TensorKernels.h"

#include <cstdint>

using nlang::native::ArgArray;
using nlang::native::Raise;
using nlang::native::ReturnDouble;
using nlang::native::ReturnFloat;
using nlang::native::ReturnInt;
using nlang::native::ReturnLong;

namespace tensor {

namespace {

enum class RedOp : int { Sum, Max, Min };

//Typed return: each carrier writes its own width into the ret cell.
void ReturnScalar(uint8_t* ret, int32_t v) { ReturnInt(ret, v); }
void ReturnScalar(uint8_t* ret, int64_t v) { ReturnLong(ret, v); }
void ReturnScalar(uint8_t* ret, float v)   { ReturnFloat(ret, v); }
void ReturnScalar(uint8_t* ret, double v)  { ReturnDouble(ret, v); }

template <typename T, typename Acc>
void UnaryReduceKernel(NativeHost* host, uint8_t* ret,
                       const uint8_t* args, RedOp op) {
    const NativeArrayView v = ArgArray(host, args, 0);
    const T* cells = CellsOf<const T>(v);
    if (op == RedOp::Sum) {
        Acc acc = 0;
        for (int64_t i = 0; i < v.elemCount; ++i)
            acc = static_cast<Acc>(acc + cells[i]);
        ReturnScalar(ret, static_cast<T>(acc));
        return;
    }
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, op == RedOp::Max
            ? "tensor.max: empty array" : "tensor.min: empty array");
    T best = cells[0];
    for (int64_t i = 1; i < v.elemCount; ++i) {
        const T cur = cells[i];
        if (op == RedOp::Max ? cur > best : cur < best)
            best = cur;
    }
    ReturnScalar(ret, best);
}

//mean: double accumulation, double return, every element kind.
template <typename T>
void MeanKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    const NativeArrayView v = ArgArray(host, args, 0);
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, "tensor.mean: empty array");
    const T* cells = CellsOf<const T>(v);
    double acc = 0.0;
    for (int64_t i = 0; i < v.elemCount; ++i)
        acc += static_cast<double>(cells[i]);
    ReturnDouble(ret, acc / static_cast<double>(v.elemCount));
}

//dot: same-kind operands, 64-bit accumulation for integral kinds (the
//declared return widens with it); float accumulates float, double double.
template <typename T, typename Acc>
void DotKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    const NativeArrayView va = ArgArray(host, args, 0);
    const NativeArrayView vb = ArgArray(host, args, 1);
    if (va.elemKind != vb.elemKind)
        Raise(host, NEXC_Base, "tensor: element type mismatch");
    if (va.elemCount != vb.elemCount)
        Raise(host, NEXC_Base, "tensor: length mismatch");
    const T* ca = CellsOf<const T>(va);
    const T* cb = CellsOf<const T>(vb);
    Acc acc = 0;
    for (int64_t i = 0; i < va.elemCount; ++i)
        acc = static_cast<Acc>(acc + static_cast<Acc>(ca[i])
            * static_cast<Acc>(cb[i]));
    ReturnScalar(ret, acc);
}

//Kind dispatch on the first view (same contract as the binary family).
uint8_t ElemKindOfFirst(NativeHost* host, const uint8_t* args) {
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v);
    return v.elemKind;
}

} // namespace

void TensorSum(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            UnaryReduceKernel<int32_t, int64_t>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t, int64_t>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float, double>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double, double>(host, ret, args, RedOp::Sum);
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorMax(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            UnaryReduceKernel<int32_t, int64_t>(host, ret, args, RedOp::Max);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t, int64_t>(host, ret, args, RedOp::Max);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float, double>(host, ret, args, RedOp::Max);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double, double>(host, ret, args, RedOp::Max);
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorMin(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            UnaryReduceKernel<int32_t, int64_t>(host, ret, args, RedOp::Min);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t, int64_t>(host, ret, args, RedOp::Min);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float, double>(host, ret, args, RedOp::Min);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double, double>(host, ret, args, RedOp::Min);
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorMean(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:   MeanKernel<int32_t>(host, ret, args); break;
        case NEK_Int64:   MeanKernel<int64_t>(host, ret, args); break;
        case NEK_Float32: MeanKernel<float>(host, ret, args); break;
        case NEK_Float64: MeanKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorDot(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:   DotKernel<int32_t, int64_t>(host, ret, args); break;
        case NEK_Int64:   DotKernel<int64_t, int64_t>(host, ret, args); break;
        case NEK_Float32: DotKernel<float, float>(host, ret, args); break;
        case NEK_Float64: DotKernel<double, double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

} // namespace tensor
