/*---
    tensor_binary.cpp — the elementwise binary family (add/sub/mul/div,
    the scalar arms, the reversed arms), the unary math pair (exp/log)
    and unary minus. P1 T1.2a migration: the dispatcher keeps kind
    dispatch, validation, broadcast-shape computation and output
    minting; the RUN routes per call by the primary operand's residency (OpsForResidency — host operands run the cpu table even under a device backend) (the CPU backend
    passes the pinned views straight to the existing tier-dispatched
    drivers — zero behavior change; device backends implement the same
    slots as device kernels).
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <cstdint>
#include <type_traits>

using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

enum class BinOp : int { Add, Sub, Mul, Div };

//BinOp and BinaryKernelOp share the Add/Sub/Mul/Div values (same enum
//order, asserted by the TensorOps contract comment).
constexpr BinaryKernelOp ToBin(BinOp op) {
    return static_cast<BinaryKernelOp>(op);
}

//Broadcasting elementwise arm: validate, mint, route. The slot runs
//the flat tier kernels for contiguous floats and the broadcast odometer
//otherwise (ints always walk — wrap-and-raise semantics).
template <typename T>
void BinaryKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  uint8_t kind, BinOp op) {
    NativeArrayView va;
    NativeArrayView vb;
    ArgArray(host, args, 0, &va);
    ArgArray(host, args, 1, &vb);
    if (va.elemKind != vb.elemKind)
        Raise(host, NEXC_Base, "tensor: element type mismatch");
    if (va.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    const NativeArrayView operands[] = {va, vb};
    AssertSameResidency(host, operands, 2);
    int32_t outDims[nlang::kMaxArrayRank];
    const int32_t outRank = BroadcastShapes(host, va, vb, outDims);
    const int32_t handle = MintOutputLike(host, kind, outRank, outDims, va);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const TensorOps& ops = OpsForResidency(va.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.binaryF32(host, ToBin(op), va, vb, vout);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.binaryF64(host, ToBin(op), va, vb, vout);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.binaryI32(host, ToBin(op), va, vb, vout);
    } else {
        ops.binaryI64(host, ToBin(op), va, vb, vout);
    }
    ReturnInt(ret, handle);
}

//Scalar arm: the operand shape is kept; the scalar rides slot 1 in the
//kind's own carrier (the NLang overload picked it statically). The
//zero-scalar div raise fires before any work, exactly as before.
template <typename T>
void ScalarKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  uint8_t kind, BinOp op, T scalar) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    if constexpr (std::is_integral_v<T>) {
        if (op == BinOp::Div && scalar == T(0))
            Raise(host, NEXC_DivideByZero,
                  "tensor.div: integer division by zero");
    }
    const int32_t handle = MintOutputLike(host, kind, v.rank, v.dims, v);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const TensorOps& ops = OpsForResidency(v.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.binaryScalarF32(host, ToBin(op), v, scalar, vout);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.binaryScalarF64(host, ToBin(op), v, scalar, vout);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.binaryScalarI32(host, ToBin(op), v, scalar, vout);
    } else {
        ops.binaryScalarI64(host, ToBin(op), v, scalar, vout);
    }
    ReturnInt(ret, handle);
}

//Kind dispatch on the first view's runtime element kind; the declared
//overload set gates the reachable kinds, the default arm is defensive.
void DispatchBinary(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    BinOp op) {
    NativeArrayView va;
    va.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &va);
    switch (va.elemKind) {
        case NEK_Int32:
            BinaryKernel<int32_t>(host, ret, args, NEK_Int32, op); break;
        case NEK_Int64:
            BinaryKernel<int64_t>(host, ret, args, NEK_Int64, op); break;
        case NEK_Float32:
            BinaryKernel<float>(host, ret, args, NEK_Float32, op); break;
        case NEK_Float64:
            BinaryKernel<double>(host, ret, args, NEK_Float64, op); break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void DispatchScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    BinOp op) {
    NativeArrayView va;
    va.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &va);
    switch (va.elemKind) {
        case NEK_Int32:
            ScalarKernel<int32_t>(host, ret, args, NEK_Int32, op,
                                  ArgInt(args, 1));
            break;
        case NEK_Int64:
            ScalarKernel<int64_t>(host, ret, args, NEK_Int64, op,
                                  ArgLong(args, 1));
            break;
        case NEK_Float32:
            ScalarKernel<float>(host, ret, args, NEK_Float32, op,
                                ArgFloat(args, 1));
            break;
        case NEK_Float64:
            ScalarKernel<double>(host, ret, args, NEK_Float64, op,
                                 ArgDouble(args, 1));
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

//Unary math maps: rank-preserving, stride-aware (a view reads through
//its recorded layout); only float/double reach here (the dispatch
//raises other kinds).
enum class UnaryOp : int { Exp, Log };

template <typename T>
void UnaryMathKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     uint8_t kind, UnaryOp op) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    const int32_t handle = MintOutputLike(host, kind, v.rank, v.dims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(v.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.unaryF32(host, op == UnaryOp::Exp ? UnaryKernelOp::Exp
                                              : UnaryKernelOp::Log,
                     v, out);
    } else {
        ops.unaryF64(host, op == UnaryOp::Exp ? UnaryKernelOp::Exp
                                              : UnaryKernelOp::Log,
                     v, out);
    }
    ReturnInt(ret, handle);
}

//Kind dispatch for the unary pair; integer kinds are a defensive raise
//(the declared surface only admits float/double).
void DispatchUnaryMath(NativeHost* host, uint8_t* ret,
                       const uint8_t* args, UnaryOp op) {
    NativeArrayView va;
    va.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &va);
    switch (va.elemKind) {
        case NEK_Float32:
            UnaryMathKernel<float>(host, ret, args, NEK_Float32, op);
            break;
        case NEK_Float64:
            UnaryMathKernel<double>(host, ret, args, NEK_Float64, op);
            break;
        default:
            Raise(host, NEXC_Base,
                  "tensor: element kind has no exp/log semantics");
    }
}

} // namespace

#define TENSOR_BINARY(Name, Op)                                          \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,       \
              int argc) {                                                \
        (void)argc;                                                      \
        DispatchBinary(host, ret, args, (Op));                           \
    }

#define TENSOR_SCALAR(Name, Op)                                          \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,       \
              int argc) {                                                \
        (void)argc;                                                      \
        DispatchScalar(host, ret, args, (Op));                           \
    }

TENSOR_BINARY(TensorAdd, BinOp::Add)
TENSOR_BINARY(TensorSub, BinOp::Sub)
TENSOR_BINARY(TensorMul, BinOp::Mul)
TENSOR_BINARY(TensorDiv, BinOp::Div)
TENSOR_SCALAR(TensorAddScalar, BinOp::Add)
TENSOR_SCALAR(TensorSubScalar, BinOp::Sub)
TENSOR_SCALAR(TensorMulScalar, BinOp::Mul)
TENSOR_SCALAR(TensorDivScalar, BinOp::Div)

//Elementwise unary minus: int kinds wrap like language arithmetic in
//the scalar walk; float/double ride the tier-dispatched neg lane.
template <typename T>
void NegKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
               uint8_t kind) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    const int32_t handle = MintOutputLike(host, kind, v.rank, v.dims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(v.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.unaryF32(host, UnaryKernelOp::Neg, v, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.unaryF64(host, UnaryKernelOp::Neg, v, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.unaryI32(host, UnaryKernelOp::Neg, v, out);
    } else {
        ops.unaryI64(host, UnaryKernelOp::Neg, v, out);
    }
    ReturnInt(ret, handle);
}

void TensorNeg(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    (void)argc;
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Int32:
            NegKernel<int32_t>(host, ret, args, NEK_Int32); break;
        case NEK_Int64:
            NegKernel<int64_t>(host, ret, args, NEK_Int64); break;
        case NEK_Float32:
            NegKernel<float>(host, ret, args, NEK_Float32); break;
        case NEK_Float64:
            NegKernel<double>(host, ret, args, NEK_Float64); break;
        default:
            Raise(host, NEXC_Base,
                  "tensor: element kind has no neg semantics");
    }
}

//Reversed scalar arms: s - a (rsub) and s / a (rdiv) with the scalar on
//the LEFT — mapped onto the Rsub/Rdiv slot ops. The zero-scalar div
//raise stays here (before the mint, exactly as before); the per-cell
//semantics ride the slots.
template <typename T>
void ReversedScalarKernel(NativeHost* host, uint8_t* ret,
                          const uint8_t* args, uint8_t kind, BinOp op,
                          T scalar) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    if constexpr (std::is_integral_v<T>) {
        if (op == BinOp::Div && scalar == T(0))
            Raise(host, NEXC_DivideByZero,
                  "tensor.div: integer division by zero");
    }
    const int32_t handle = MintOutputLike(host, kind, v.rank, v.dims, v);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const BinaryKernelOp binOp = op == BinOp::Sub
        ? BinaryKernelOp::Rsub : BinaryKernelOp::Rdiv;
    const TensorOps& ops = OpsForResidency(v.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.binaryScalarF32(host, binOp, v, scalar, vout);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.binaryScalarF64(host, binOp, v, scalar, vout);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.binaryScalarI32(host, binOp, v, scalar, vout);
    } else {
        ops.binaryScalarI64(host, binOp, v, scalar, vout);
    }
    ReturnInt(ret, handle);
}

void TensorRsubScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                      int argc) {
    (void)argc;
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Int32:
            ReversedScalarKernel<int32_t>(host, ret, args, NEK_Int32,
                                          BinOp::Sub, ArgInt(args, 1));
            break;
        case NEK_Int64:
            ReversedScalarKernel<int64_t>(host, ret, args, NEK_Int64,
                                          BinOp::Sub, ArgLong(args, 1));
            break;
        case NEK_Float32:
            ReversedScalarKernel<float>(host, ret, args, NEK_Float32,
                                        BinOp::Sub, ArgFloat(args, 1));
            break;
        case NEK_Float64:
            ReversedScalarKernel<double>(host, ret, args, NEK_Float64,
                                         BinOp::Sub, ArgDouble(args, 1));
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorRdivScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                      int argc) {
    (void)argc;
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Int32:
            ReversedScalarKernel<int32_t>(host, ret, args, NEK_Int32,
                                          BinOp::Div, ArgInt(args, 1));
            break;
        case NEK_Int64:
            ReversedScalarKernel<int64_t>(host, ret, args, NEK_Int64,
                                          BinOp::Div, ArgLong(args, 1));
            break;
        case NEK_Float32:
            ReversedScalarKernel<float>(host, ret, args, NEK_Float32,
                                        BinOp::Div, ArgFloat(args, 1));
            break;
        case NEK_Float64:
            ReversedScalarKernel<double>(host, ret, args, NEK_Float64,
                                         BinOp::Div, ArgDouble(args, 1));
            break;
        default:
            Raise(host, NEXC_Base, "tensor: unsupported element kind");
    }
}

void TensorExp(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    (void)argc;
    DispatchUnaryMath(host, ret, args, UnaryOp::Exp);
}

void TensorLog(NativeHost* host, uint8_t* ret, const uint8_t* args,
               int argc) {
    (void)argc;
    DispatchUnaryMath(host, ret, args, UnaryOp::Log);
}

} // namespace tensor
