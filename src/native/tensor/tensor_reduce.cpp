/*---
    tensor_reduce.cpp — the reduction family (sum/max/min/mean/dot) of
    the tensor native module. Same dispatch shape as the binary family:
    one registered native per operation name, the first view's runtime
    element kind picks the typed kernel. max/min also carry the clamp
    arm (array + bound, arity-dispatched on argc); sum carries the axis
    arm (array + axis). Every walk addresses through the recorded
    strides/offset, so views reduce without materializing. Integer sums
    wrap like language int arithmetic (declared T return); dot
    accumulates in 64 bits and returns long so large inner products
    never wrap; mean always returns double.
---*/
#include "TensorKernels.h"

#include <cstdint>
#include <string>

using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::NewArrayShaped;
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
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const T* cells = CellsOf<const T>(v);
    if (op == RedOp::Sum) {
        Acc acc = 0;
        StrideWalk(v, [&](int64_t addr) {
            acc = static_cast<Acc>(acc + cells[addr]);
        });
        ReturnScalar(ret, static_cast<T>(acc));
        return;
    }
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, op == RedOp::Max
            ? "tensor.max: empty array" : "tensor.min: empty array");
    T best = T(0);
    bool first = true;
    StrideWalk(v, [&](int64_t addr) {
        const T cur = cells[addr];
        if (first || (op == RedOp::Max ? cur > best : cur < best)) {
            best = cur;
            first = false;
        }
    });
    ReturnScalar(ret, best);
}

//mean: double accumulation, double return, every element kind.
template <typename T>
void MeanKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, "tensor.mean: empty array");
    const T* cells = CellsOf<const T>(v);
    double acc = 0.0;
    StrideWalk(v, [&](int64_t addr) {
        acc += static_cast<double>(cells[addr]);
    });
    ReturnDouble(ret, acc / static_cast<double>(v.elemCount));
}

//dot: same-kind operands, 64-bit accumulation for integral kinds (the
//declared return widens with it); float accumulates float, double
//double. Both views walk their own row-major odometer — the shapes may
//differ as long as the counts match (2x3 . 3x2 flattens alike).
template <typename T, typename Acc>
void DotKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView va;
    NativeArrayView vb;
    ArgArray(host, args, 0, &va);
    ArgArray(host, args, 1, &vb);
    if (va.elemKind != vb.elemKind)
        Raise(host, NEXC_Base, "tensor: element type mismatch");
    if (va.elemCount != vb.elemCount)
        Raise(host, NEXC_Base, "tensor: length mismatch ("
            + std::to_string(va.elemCount) + " vs "
            + std::to_string(vb.elemCount) + ")");
    const T* ca = CellsOf<const T>(va);
    const T* cb = CellsOf<const T>(vb);
    int64_t idxA[nlang::kMaxArrayRank] = {0};
    int64_t idxB[nlang::kMaxArrayRank] = {0};
    int64_t addrA = va.offset;
    int64_t addrB = vb.offset;
    Acc acc = 0;
    for (int64_t i = 0; i < va.elemCount; ++i) {
        acc = static_cast<Acc>(acc + static_cast<Acc>(ca[addrA])
            * static_cast<Acc>(cb[addrB]));
        StrideWalkAdvance(va, idxA, &addrA);
        StrideWalkAdvance(vb, idxB, &addrB);
    }
    ReturnScalar(ret, acc);
}

//Clamp arm of max/min (array + bound, argc >= 2): elementwise bound,
//operand shape preserved.
template <typename T>
void ClampKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 bool isMax, T bound) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t handle = NewArrayShaped(host, v.elemKind, v.rank, v.dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const T* in = CellsOf<const T>(v);
    T* outCells = CellsOf<T>(out);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        const T cur = in[addr];
        outCells[at++] = isMax ? (cur > bound ? cur : bound)
                               : (cur < bound ? cur : bound);
    });
    ReturnInt(ret, handle);
}

//Kind dispatch for the clamp arm; the bound rides slot 1 in the kind's
//own carrier.
void DispatchClamp(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   bool isMax) {
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Int32:
            ClampKernel(host, ret, args, isMax, ArgInt(args, 1));
            break;
        case NEK_Int64:
            ClampKernel(host, ret, args, isMax, ArgLong(args, 1));
            break;
        case NEK_Float32:
            ClampKernel(host, ret, args, isMax, ArgFloat(args, 1));
            break;
        case NEK_Float64:
            ClampKernel(host, ret, args, isMax, ArgDouble(args, 1));
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

//sum(a, axis): the axis dim is eliminated, every other dim is kept
//(rank 1 + axis 0 is the full reduction — the scalar return). The
//outer odometer walks the result coords; the inner loop strides along
//the source axis. Accumulation mirrors the flattened sum (Acc carrier,
//narrowed to T on store).
template <typename T, typename Acc>
void SumAxisKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t axis = ArgInt(args, 1);
    if (axis < 0 || axis >= v.rank)
        Raise(host, NEXC_Base, "tensor.sum: axis "
              + std::to_string(axis) + " out of range for rank "
              + std::to_string(v.rank));
    const T* cells = CellsOf<const T>(v);
    const int32_t along = v.dims[axis];
    const int64_t axisStride = v.strides[axis];
    const int32_t outRank = v.rank - 1;
    if (outRank == 0) {
        Acc acc = 0;
        for (int64_t s = 0; s < along; ++s)
            acc = static_cast<Acc>(acc + cells[v.offset + s * axisStride]);
        ReturnScalar(ret, static_cast<T>(acc));
        return;
    }
    int32_t outDims[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < outRank; ++d)
        outDims[d] = v.dims[d < axis ? d : d + 1];
    const int32_t handle = NewArrayShaped(host, v.elemKind, outRank,
                                          outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    T* outCells = CellsOf<T>(out);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t base = v.offset;
    for (int64_t i = 0; i < out.elemCount; ++i) {
        Acc acc = 0;
        for (int64_t s = 0; s < along; ++s)
            acc = static_cast<Acc>(acc + cells[base + s * axisStride]);
        outCells[i] = static_cast<T>(acc);
        for (int32_t d = outRank - 1; d >= 0; --d) {
            const int32_t src = d < axis ? d : d + 1;
            idx[d] += 1;
            base += v.strides[src];
            if (idx[d] < outDims[d])
                break;
            base -= static_cast<int64_t>(outDims[d]) * v.strides[src];
            idx[d] = 0;
        }
    }
    ReturnInt(ret, handle);
}

//Kind dispatch on the first view (same contract as the binary family).
uint8_t ElemKindOfFirst(NativeHost* host, const uint8_t* args) {
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v);
    return v.elemKind;
}

} // namespace

void TensorSum(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    if (argc == 2) {
        switch (ElemKindOfFirst(host, args)) {
            case NEK_Int32:
                SumAxisKernel<int32_t, int64_t>(host, ret, args); break;
            case NEK_Int64:
                SumAxisKernel<int64_t, int64_t>(host, ret, args); break;
            case NEK_Float32:
                SumAxisKernel<float, double>(host, ret, args); break;
            case NEK_Float64:
                SumAxisKernel<double, double>(host, ret, args); break;
            default:
                Raise(host, NEXC_Base, "tensor: unsupported element kind");
        }
        return;
    }
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

void TensorMax(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    if (argc >= 2) {
        DispatchClamp(host, ret, args, true);
        return;
    }
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

void TensorMin(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    if (argc >= 2) {
        DispatchClamp(host, ret, args, false);
        return;
    }
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

void TensorMean(NativeHost* host, uint8_t* ret, const uint8_t* args,
                int argc) {
    (void)argc;
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:   MeanKernel<int32_t>(host, ret, args); break;
        case NEK_Int64:   MeanKernel<int64_t>(host, ret, args); break;
        case NEK_Float32: MeanKernel<float>(host, ret, args); break;
        case NEK_Float64: MeanKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorDot(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    (void)argc;
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            DotKernel<int32_t, int64_t>(host, ret, args); break;
        case NEK_Int64:
            DotKernel<int64_t, int64_t>(host, ret, args); break;
        case NEK_Float32:
            DotKernel<float, float>(host, ret, args); break;
        case NEK_Float64:
            DotKernel<double, double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

} // namespace tensor
