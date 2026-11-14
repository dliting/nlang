/*---
    tensor_reduce.cpp — the reduction family (sum/max/min/mean/dot) of
    the tensor native module. P1 T1.2b migration: the dispatcher keeps
    arity dispatch, kind dispatch, validation (axis range, empty
    arrays, the rank-1 axis-arm raise, count mismatches), the rank-1
    scalar-return arms and output minting; the run rides the
    ActiveTensorOps() slots (sumAll/dot/extreme/mean/clamp/reduceAxis).
    Integer sums wrap like language int arithmetic (declared T return);
    dot accumulates in 64 bits for ints and returns long; mean always
    returns double (int kinds accumulate double — never wraps).
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

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

template <typename T>
void UnaryReduceKernel(NativeHost* host, uint8_t* ret,
                       const uint8_t* args, RedOp op) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (op != RedOp::Sum && v.elemCount == 0)
        Raise(host, NEXC_Base, op == RedOp::Max
            ? "tensor.max: empty array" : "tensor.min: empty array");
    const TensorOps& ops = ActiveTensorOps();
    if (op == RedOp::Sum) {
        double acc = 0.0;
        int64_t accI = 0;
        if constexpr (std::is_same_v<T, float>) {
            ops.sumAllF32(host, v, &acc);
            ReturnScalar(ret, static_cast<T>(acc));
        } else if constexpr (std::is_same_v<T, double>) {
            ops.sumAllF64(host, v, &acc);
            ReturnScalar(ret, static_cast<T>(acc));
        } else if constexpr (std::is_same_v<T, int32_t>) {
            ops.sumAllI32(host, v, &accI);
            ReturnScalar(ret, static_cast<T>(accI));
        } else {
            ops.sumAllI64(host, v, &accI);
            ReturnScalar(ret, static_cast<T>(accI));
        }
        return;
    }
    const bool isMax = op == RedOp::Max;
    if constexpr (std::is_same_v<T, float>) {
        float best = 0.0f;
        ops.extremeF32(host, isMax, v, &best);
        ReturnScalar(ret, best);
    } else if constexpr (std::is_same_v<T, double>) {
        double best = 0.0;
        ops.extremeF64(host, isMax, v, &best);
        ReturnScalar(ret, best);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        int32_t best = 0;
        ops.extremeI32(host, isMax, v, &best);
        ReturnScalar(ret, best);
    } else {
        int64_t best = 0;
        ops.extremeI64(host, isMax, v, &best);
        ReturnScalar(ret, best);
    }
}

//mean: double accumulation, double return, every element kind. The
//slot writes the mean (it divides by the count); the dispatcher returns
//it as-is.
template <typename T>
void MeanKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, "tensor.mean: empty array");
    double mean = 0.0;
    const TensorOps& ops = ActiveTensorOps();
    if constexpr (std::is_same_v<T, float>) {
        ops.meanF32(host, v, &mean);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.meanF64(host, v, &mean);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.meanI32(host, v, &mean);
    } else {
        ops.meanI64(host, v, &mean);
    }
    ReturnDouble(ret, mean);
}

//dot: same-kind operands, 64-bit accumulation for integral kinds (the
//declared return widens with it); float accumulates float, double
//double.
template <typename T>
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
    {
        const NativeArrayView operands[] = {va, vb};
        AssertSameResidency(host, operands, 2);
    }
    const TensorOps& ops = ActiveTensorOps();
    if constexpr (std::is_same_v<T, float>) {
        float acc = 0.0f;
        ops.dotF32(host, va, vb, &acc);
        ReturnScalar(ret, acc);
    } else if constexpr (std::is_same_v<T, double>) {
        double acc = 0.0;
        ops.dotF64(host, va, vb, &acc);
        ReturnScalar(ret, acc);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        int64_t acc = 0;
        ops.dotI32(host, va, vb, &acc);
        ReturnScalar(ret, acc);
    } else {
        int64_t acc = 0;
        ops.dotI64(host, va, vb, &acc);
        ReturnScalar(ret, acc);
    }
}

//Clamp arm of max/min (array + bound, argc >= 2): elementwise bound,
//operand shape preserved.
template <typename T>
void ClampKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 bool isMax, T bound) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t handle = MintOutputLike(host, v.elemKind, v.rank, v.dims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = ActiveTensorOps();
    if constexpr (std::is_same_v<T, float>) {
        ops.clampF32(host, isMax, v, bound, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.clampF64(host, isMax, v, bound, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.clampI32(host, isMax, v, bound, out);
    } else {
        ops.clampI64(host, isMax, v, bound, out);
    }
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
//dispatcher keeps the axis-range validation and the scalar-return arm;
//the array arm mints and routes through reduceAxis.
template <typename T>
void SumAxisKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t axis = ArgInt(args, 1);
    if (axis < 0 || axis >= v.rank)
        Raise(host, NEXC_Base, "tensor.sum: axis "
              + std::to_string(axis) + " out of range for rank "
              + std::to_string(v.rank));
    const int32_t outRank = v.rank - 1;
    if (outRank == 0) {
        //Rank-1 + axis 0: the full reduction (the scalar return).
        double acc = 0.0;
        int64_t accI = 0;
        const TensorOps& ops = ActiveTensorOps();
        if constexpr (std::is_same_v<T, float>) {
            ops.sumAllF32(host, v, &acc);
            ReturnScalar(ret, static_cast<T>(acc));
        } else if constexpr (std::is_same_v<T, double>) {
            ops.sumAllF64(host, v, &acc);
            ReturnScalar(ret, static_cast<T>(acc));
        } else if constexpr (std::is_same_v<T, int32_t>) {
            ops.sumAllI32(host, v, &accI);
            ReturnScalar(ret, static_cast<T>(accI));
        } else {
            ops.sumAllI64(host, v, &accI);
            ReturnScalar(ret, static_cast<T>(accI));
        }
        return;
    }
    int32_t outDims[nlang::kMaxArrayRank];
    AxisRemovedDims(v, axis, outDims);
    const int32_t handle = MintOutputLike(host, v.elemKind, outRank,
                                          outDims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = ActiveTensorOps();
    if constexpr (std::is_same_v<T, float>) {
        ops.reduceAxisF32(host, ReduceAxisOp::Sum, v, axis, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.reduceAxisF64(host, ReduceAxisOp::Sum, v, axis, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.reduceAxisI32(host, ReduceAxisOp::Sum, v, axis, out);
    } else {
        ops.reduceAxisI64(host, ReduceAxisOp::Sum, v, axis, out);
    }
    ReturnInt(ret, handle);
}

//Axis arm of max/min: the axis dim is eliminated, every other dim kept
//(rank-1 + axis 0 is the full reduction — the scalar return, which the
//family routes to max(a)/min(a) with a named raise). The dispatcher
//keeps the axis/empty/rank-1 validations; the arm mints and routes.
template <typename T>
void ExtremeAxisKernel(NativeHost* host, uint8_t* ret,
                       const uint8_t* args, bool isMax) {
    const char* who = isMax ? "tensor.max" : "tensor.min";
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t axis = ArgInt(args, 1);
    if (axis < 0 || axis >= v.rank)
        Raise(host, NEXC_Base, who + std::string(": axis ")
              + std::to_string(axis) + " out of range for rank "
              + std::to_string(v.rank));
    //The family-consistent empty guard (the scalar reductions and
    //argmax/argmin raise the same way): a zero-size source has no
    //position to start the scan from — cells[base] below reads past
    //the allocation when the axis dim itself is zero.
    if (v.elemCount == 0)
        Raise(host, NEXC_Base, std::string(who) + ": empty array");
    const int32_t outRank = v.rank - 1;
    if (outRank == 0)
        Raise(host, NEXC_Base, std::string(who)
              + ": rank-1 arrays reduce with "
              + (isMax ? "max(a)" : "min(a)")
              + ", not an axis arm");
    //The static return is an Array; a rank-1 receiver would have to
    //come back as a scalar, so the axis arm requires rank >= 2.
    int32_t outDims[nlang::kMaxArrayRank];
    AxisRemovedDims(v, axis, outDims);
    const int32_t handle = MintOutputLike(host, v.elemKind, outRank,
                                          outDims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = ActiveTensorOps();
    if constexpr (std::is_same_v<T, float>) {
        ops.reduceAxisF32(host, isMax ? ReduceAxisOp::Max
                                      : ReduceAxisOp::Min,
                          v, axis, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.reduceAxisF64(host, isMax ? ReduceAxisOp::Max
                                      : ReduceAxisOp::Min,
                          v, axis, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.reduceAxisI32(host, isMax ? ReduceAxisOp::Max
                                      : ReduceAxisOp::Min,
                          v, axis, out);
    } else {
        ops.reduceAxisI64(host, isMax ? ReduceAxisOp::Max
                                      : ReduceAxisOp::Min,
                          v, axis, out);
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

void TensorMaxAxis(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            ExtremeAxisKernel<int32_t>(host, ret, args, true); break;
        case NEK_Int64:
            ExtremeAxisKernel<int64_t>(host, ret, args, true); break;
        case NEK_Float32:
            ExtremeAxisKernel<float>(host, ret, args, true); break;
        case NEK_Float64:
            ExtremeAxisKernel<double>(host, ret, args, true); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorMinAxis(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            ExtremeAxisKernel<int32_t>(host, ret, args, false); break;
        case NEK_Int64:
            ExtremeAxisKernel<int64_t>(host, ret, args, false); break;
        case NEK_Float32:
            ExtremeAxisKernel<float>(host, ret, args, false); break;
        case NEK_Float64:
            ExtremeAxisKernel<double>(host, ret, args, false); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorSum(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    if (argc == 2) {
        switch (ElemKindOfFirst(host, args)) {
            case NEK_Int32:
                SumAxisKernel<int32_t>(host, ret, args); break;
            case NEK_Int64:
                SumAxisKernel<int64_t>(host, ret, args); break;
            case NEK_Float32:
                SumAxisKernel<float>(host, ret, args); break;
            case NEK_Float64:
                SumAxisKernel<double>(host, ret, args); break;
            default:
                Raise(host, NEXC_Base, "tensor: unsupported element kind");
        }
        return;
    }
    switch (ElemKindOfFirst(host, args)) {
        case NEK_Int32:
            UnaryReduceKernel<int32_t>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float>(host, ret, args, RedOp::Sum);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double>(host, ret, args, RedOp::Sum);
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
            UnaryReduceKernel<int32_t>(host, ret, args, RedOp::Max);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t>(host, ret, args, RedOp::Max);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float>(host, ret, args, RedOp::Max);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double>(host, ret, args, RedOp::Max);
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
            UnaryReduceKernel<int32_t>(host, ret, args, RedOp::Min);
            break;
        case NEK_Int64:
            UnaryReduceKernel<int64_t>(host, ret, args, RedOp::Min);
            break;
        case NEK_Float32:
            UnaryReduceKernel<float>(host, ret, args, RedOp::Min);
            break;
        case NEK_Float64:
            UnaryReduceKernel<double>(host, ret, args, RedOp::Min);
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
            DotKernel<int32_t>(host, ret, args); break;
        case NEK_Int64:
            DotKernel<int64_t>(host, ret, args); break;
        case NEK_Float32:
            DotKernel<float>(host, ret, args); break;
        case NEK_Float64:
            DotKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

} // namespace tensor
