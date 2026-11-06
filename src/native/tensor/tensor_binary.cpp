/*---
    tensor_binary.cpp — the elementwise binary family (add/sub/mul/div)
    and the unary math pair (exp/log) of the tensor native module. The
    array/array arm broadcasts (right-aligned rule; the result is an
    owning record of the broadcast shape), the scalar arm keeps the
    operand shape; both address operands through their recorded
    strides/offset, so views read correctly without materializing. One
    registered native per operation name — the NLang side declares the
    kind overloads and resolution funnels them here; the runtime element
    kind of the first view picks the typed kernel. exp/log stay
    rank-preserving float/double maps.
---*/
#include "TensorKernels.h"

#include <cmath>
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

template <typename T>
T ApplyBin(BinOp op, T a, T b) {
    switch (op) {
        case BinOp::Add: return static_cast<T>(a + b);
        case BinOp::Sub: return static_cast<T>(a - b);
        case BinOp::Mul: return static_cast<T>(a * b);
        case BinOp::Div:
            //MIN/-1 traps x86 idiv (same fix as the language rows):
            //wrap through the unsigned carrier — negation's two's-
            //complement result.
            if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                if (b == T(-1)) {
                    using U = std::make_unsigned_t<T>;
                    return static_cast<T>(static_cast<U>(0)
                        - static_cast<U>(a));
                }
            }
            return static_cast<T>(a / b);
    }
    return T(0);
}

//Broadcasting elementwise kernel. The output walks its own row-major
//odometer; each operand addresses through padded broadcast strides
//(size-1 and missing leading axes repeat their single cell). Integer
//division by zero raises DivByZeroException when the DIVIDEND's cell is
//reached, mirroring the language rows.
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
    int32_t outDims[nlang::kMaxArrayRank];
    const int32_t outRank = BroadcastShapes(host, va, vb, outDims);
    int32_t sa[nlang::kMaxArrayRank];
    int32_t sb[nlang::kMaxArrayRank];
    BroadcastStrides(va, outRank, sa);
    BroadcastStrides(vb, outRank, sb);
    const int32_t handle = NewArrayShaped(host, kind, outRank, outDims);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const T* ca = CellsOf<const T>(va);
    const T* cb = CellsOf<const T>(vb);
    T* cout = CellsOf<T>(vout);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addA = va.offset;
    int64_t addB = vb.offset;
    for (int64_t i = 0; i < vout.elemCount; ++i) {
        if constexpr (std::is_integral_v<T>) {
            if (op == BinOp::Div && cb[addB] == T(0))
                Raise(host, NEXC_DivideByZero,
                      "tensor.div: integer division by zero");
        }
        cout[i] = ApplyBin<T>(op, ca[addA], cb[addB]);
        for (int32_t d = outRank - 1; d >= 0; --d) {
            idx[d] += 1;
            addA += sa[d];
            addB += sb[d];
            if (idx[d] < outDims[d])
                break;
            addA -= static_cast<int64_t>(outDims[d]) * sa[d];
            addB -= static_cast<int64_t>(outDims[d]) * sb[d];
            idx[d] = 0;
        }
    }
    ReturnInt(ret, handle);
}

//Scalar arm: the operand shape is kept; the scalar rides slot 1 in the
//kind's own carrier (the NLang overload picked it statically).
template <typename T>
void ScalarKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  uint8_t kind, BinOp op, T scalar) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    const int32_t handle = NewArrayShaped(host, kind, v.rank, v.dims);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const T* cells = CellsOf<const T>(v);
    T* cout = CellsOf<T>(vout);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        if constexpr (std::is_integral_v<T>) {
            if (op == BinOp::Div && scalar == T(0))
                Raise(host, NEXC_DivideByZero,
                      "tensor.div: integer division by zero");
        }
        cout[at++] = ApplyBin<T>(op, cells[addr], scalar);
    });
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
//its recorded layout). std::exp/std::log overload per carrier, so one
//typed body serves float and double.
enum class UnaryOp : int { Exp, Log };

template <typename T>
void UnaryMathKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     uint8_t kind, UnaryOp op) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != kind)
        Raise(host, NEXC_Base, "tensor: unexpected element kind");
    const int32_t handle = NewArrayShaped(host, kind, v.rank, v.dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const T* in = CellsOf<const T>(v);
    T* outCells = CellsOf<T>(out);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        outCells[at++] = op == UnaryOp::Exp
            ? static_cast<T>(std::exp(in[addr]))
            : static_cast<T>(std::log(in[addr]));
    });
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
