/*---
    tensor_matrix.cpp — concat and matmul of the `tensor` native
    module. P1 T1.2c migration: the dispatcher keeps operand
    materialization (non-contiguous views copy to dense records first —
    the kernels keep flat addressing), shape gates and output minting;
    the run rides the ActiveTensorOps() slots (matmul forwards to the
    KernelOps tier table; concat's byte-level copy is kind-independent).
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

//A view of the operand, materialized to a dense owning record when its
//strides are not row-major dense (the copy path needs flat addressing).
//Returns the handle actually backing the computation — the original
//when it was already contiguous. The minted handle stays rooted per the
//newArray contract until this native returns.
int32_t ContiguousOperand(NativeHost* host, const uint8_t* args, int slot,
                          NativeArrayView* view) {
    ArgArray(host, args, slot, view);
    //v0.14.x device line: a CONTIGUOUS device operand passes through —
    //the slots take flat addressing and the device kernels (and the
    //mirror backend's host memory) read it directly. A NON-CONTIGUOUS
    //device operand materializes to a DEVICE-resident dense copy (the
    //copy slot's device→device arm) — StrideCopy would read the 2-cell
    //descriptor, and a host landing would break the sibling operands'
    //residency (the mixed check would fire on the materialized copy).
    if (view->reserved[0] == 1) {
        if (IsContiguous(*view))
            return ArgInt(args, slot);
        const int32_t handle = host->newArrayResident(host,
            view->elemKind, view->rank, view->dims);
        NativeArrayView dst;
        host->getArrayView(host, handle, &dst);
        const TensorOps& ops = ActiveTensorOps();
        switch (view->elemKind) {
            case NEK_Int32:  ops.copyI32(host, *view, dst); break;
            case NEK_Int64:  ops.copyI64(host, *view, dst); break;
            case NEK_Float32: ops.copyF32(host, *view, dst); break;
            case NEK_Float64: ops.copyF64(host, *view, dst); break;
            default:
                Raise(host, NEXC_Base,
                      "tensor: unsupported element kind");
        }
        view->elemKind = 0xEE;
        host->getArrayView(host, handle, view);
        return handle;
    }
    if (IsContiguous(*view))
        return ArgInt(args, slot);
    const int32_t handle = Materialize(host, *view);
    view->elemKind = 0xEE;
    host->getArrayView(host, handle, view);
    return handle;
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
        AssertSameResidency(host, operands, 2);
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
    const TensorOps& ops = ActiveTensorOps();
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
        AssertSameResidency(host, operands, 2);
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
    const TensorOps& ops = ActiveTensorOps();
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
