/*---
tensor_binary.cpp — the elementwise binary family (add/sub/mul/div) of
the tensor native module. One registered native per operation name; the
NLang side declares four same-name overloads per name (int/long/float/
double) and overload resolution funnels them all here — the runtime
element kind of the first view picks the typed kernel.
---*/
#include "TensorKernels.h"

#include <cstdint>
#include <string>
#include <type_traits>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
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

//Shape equality for the elementwise family: rank and every dim must
//match — strictly stronger than the old rank-1 length check, which is
//this rule's rank-1 special case.
bool ShapesEqual(const NativeArrayView& a, const NativeArrayView& b) {
    if (a.rank != b.rank || a.elemCount != b.elemCount)
        return false;
    for (int32_t d = 0; d < a.rank; ++d)
        if (a.dims[d] != b.dims[d])
            return false;
    return true;
}

//Integer division mirrors the language: /0 raises DivByZeroException.
//Floating division follows IEEE (Inf/NaN propagate silently).
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
    if (!ShapesEqual(va, vb))
        Raise(host, NEXC_Base, "tensor: shape mismatch ("
            + ShapeText(va) + " vs " + ShapeText(vb) + ")");
    //The output keeps the operand shape (rank-2 in, rank-2 out); for
    //rank 1 the single dim is the length, as before.
    const int32_t handle = NewArrayShaped(host, kind, va.rank, va.dims);
    NativeArrayView vout;
    host->getArrayView(host, handle, &vout);
    const T* ca = CellsOf<const T>(va);
    const T* cb = CellsOf<const T>(vb);
    T* cout = CellsOf<T>(vout);
    for (int64_t i = 0; i < va.elemCount; ++i) {
        if constexpr (std::is_integral_v<T>) {
            if (op == BinOp::Div && cb[i] == T(0))
                Raise(host, NEXC_DivideByZero,
                      "tensor.div: integer division by zero");
        }
        cout[i] = ApplyBin<T>(op, ca[i], cb[i]);
    }
    ReturnInt(ret, handle);
}

//Dispatch on the view's runtime element kind; unknown kinds are a
//defensive raise (the static overload set gates the reachable kinds).
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

} // namespace

#define TENSOR_BINARY(Name, Op)                                        \
    void Name(NativeHost* host, uint8_t* ret, const uint8_t* args,     \
              int argc) {                                              \
        (void)argc;                                                    \
        DispatchBinary(host, ret, args, (Op));                         \
    }

TENSOR_BINARY(TensorAdd, BinOp::Add)
TENSOR_BINARY(TensorSub, BinOp::Sub)
TENSOR_BINARY(TensorMul, BinOp::Mul)
TENSOR_BINARY(TensorDiv, BinOp::Div)

} // namespace tensor
