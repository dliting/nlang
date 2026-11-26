/*---
    tensor_matrix.cpp — concat and matmul of the `tensor` native
    module. P1 T1.2c migration: the dispatcher keeps operand
    materialization (non-contiguous HOST views copy to dense records
    first — the kernels keep flat addressing; device-resident
    operands pass through strided per R8), shape gates and output
    minting; the run routes per call by the primary operand's
    residency (OpsForResidency — the matmul/concat slots live on both
    tables; concat's byte-level copy is kind-independent).
    transpose/flatten/reshape live in tensor_views.cpp since v0.12.7
    Task 12.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <cstdint>
#include <string>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//An operand view for the flat-addressing kernels (matmul/concat):
//materialized to a dense owning record when its strides are not
//row-major dense — EXCEPT a device-resident view under the CUDA plugin,
//which passes through strided: the cuda matmul/concat slots fold each
//load through the recorded strides (per-load fold), so no D2D copy.
//Host-resident striding still materializes on host (the CPU matmul/
//concat slots keep flat dense addressing), and mirror device-resident
//striding still materializes D2D (the cpu table serves it). Returns the
//handle actually backing the computation — the original when it passes
//through. The minted handle stays rooted per the newArray contract
//until this native returns.
//A view of the operand, materialized to a dense owning record when its
//strides are not row-major dense (the copy path needs flat addressing).
//Returns the handle actually backing the computation — the original
//when it was already contiguous. The created handle stays rooted per the
//newArray contract until this native returns.
int32_t ContiguousOperand(NativeHost* host, const uint8_t* args, int slot,
                          NativeArrayView* view) {
    ArgArray(host, args, slot, view);
    //R8 (design 2026-11-16): device-resident operands pass through even
    //when strided — the cuda table (OpsForResidency's device arm is the
    //plugin table exactly when g_backend.ops != nullptr; under mirror it
    //is the cpu table, which still needs the dense materialization)
    //serves the strided-aware slot.
    if (view->reserved[0] == 1 && ActiveDeviceBackend().ops != nullptr)
        return ArgInt(args, slot);
    return MaterializeByResidency(host, ArgInt(args, slot), view);
}

//Concat body: equal ranks, every dim equal except the axis dim (which
//sums); an out-of-range axis raises the bounds family.
void ConcatKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView a;
    NativeArrayView b;
    ContiguousOperand(host, args, 0, &a);
    ContiguousOperand(host, args, 1, &b);
    if (a.elemKind != b.elemKind)
        Raise(host, NEXC_Base, "tensor.concat: element type mismatch");
    {
        const NativeArrayView operands[] = {a, b};
        AssertSameDevice(host, operands, 2);
    }
    if (a.rank != b.rank) {
        Raise(host, NEXC_Base, "tensor.concat: rank mismatch (" + ShapeText(a)
                                   + " vs " + ShapeText(b) + ")");
    }
    const int32_t axis = ArgInt(args, 2);
    if (axis < 0 || axis >= a.rank)
        Raise(host, NEXC_OutOfBounds,
              "tensor.concat: axis out of range");
    int32_t outDims[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < a.rank; ++d) {
        if (d == axis)
            continue;
        if (a.dims[d] != b.dims[d]) {
            Raise(host, NEXC_Base,
                  std::string("tensor.concat: shape mismatch (")
                      + ShapeText(a) + " vs " + ShapeText(b) + ")");
        }
        outDims[d] = a.dims[d];
    }
    outDims[axis] = a.dims[axis] + b.dims[axis];
    const int32_t handle = MintOutputLike(host, a.elemKind, a.rank, outDims, a);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(host, a);
    switch (a.elemKind) {
        case NEK_Int32:
            ops.concatI32(host, a, b, axis, out); break;
        case NEK_Int64:
            ops.concatI64(host, a, b, axis, out); break;
        case NEK_Float32:
            ops.concatF32(host, a, b, axis, out); break;
        case NEK_Float64:
            ops.concatF64(host, a, b, axis, out); break;
        default:
            Raise(host, NEXC_Base, "tensor.concat: unknown element kind");
    }
    ReturnInt(ret, handle);
}

//MatMul body: shape gates, then the dense flat kernel through the
//TensorOps slot (tier/ISA dispatch lives behind the CPU backend's
//KernelOps; int/long wrap around per the language-wide arithmetic
//rule, float/double follow IEEE). Operands arrive dense
//(ContiguousOperand in the entry).
template <typename T>
void MatMulTyped(NativeHost* host, uint8_t* ret, NativeArrayView& a,
                 NativeArrayView& b) {
    if (a.rank != 2 || b.rank != 2)
        Raise(host, NEXC_Base, "tensor.matmul: expects rank-2 operands");
    //The language-side overloads keep the kinds equal; the gate guards
    //the ABI surface (a mismatched pair would read b's cells through
    //a's carrier and return garbage). Same message shape as the
    //elementwise family.
    if (a.elemKind != b.elemKind)
        Raise(host, NEXC_Base, "tensor.matmul: element type mismatch");
    {
        const NativeArrayView operands[] = {a, b};
        AssertSameDevice(host, operands, 2);
    }
    const int32_t aRows = a.dims[0];
    const int32_t aCols = a.dims[1];
    const int32_t bRows = b.dims[0];
    const int32_t bCols = b.dims[1];
    if (aCols != bRows) {
        Raise(host, NEXC_Base,
              std::string("tensor.matmul: shape mismatch (")
                  + ShapeText(a) + " vs " + ShapeText(b) + ")");
    }
    const int32_t outDims[2] = {aRows, bCols};
    const int32_t handle = MintOutputLike(host, a.elemKind, 2, outDims, a);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(host, a);
    if constexpr (std::is_same_v<T, float>) {
        ops.matmulF32(host, a, b, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.matmulF64(host, a, b, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.matmulI32(host, a, b, out);
    } else {
        ops.matmulI64(host, a, b, out);
    }
    ReturnInt(ret, handle);
}

} // namespace

void TensorConcat(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc) {
    (void)argc;
    ConcatKernel(host, ret, args);
}

void TensorMatmul(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc) {
    (void)argc;
    //Kind dispatch off the first operand; a null handle raised in
    //ArgArray already. Non-contiguous operands (views) materialize —
    //the kernel keeps its flat addressing.
    NativeArrayView a;
    NativeArrayView b;
    ContiguousOperand(host, args, 0, &a);
    ContiguousOperand(host, args, 1, &b);
    switch (a.elemKind) {
        case NEK_Int32:
            MatMulTyped<int32_t>(host, ret, a, b); break;
        case NEK_Int64:
            MatMulTyped<int64_t>(host, ret, a, b); break;
        case NEK_Float32:
            MatMulTyped<float>(host, ret, a, b); break;
        case NEK_Float64:
            MatMulTyped<double>(host, ret, a, b); break;
        default:
            Raise(host, NEXC_Base, "tensor.matmul: unknown element kind");
    }
}

} // namespace tensor
