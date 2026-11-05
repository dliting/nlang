/*---
    tensor_matrix.cpp — concat and matmul of the `tensor` native
    module. Both copy into fresh owning records, so a non-contiguous
    operand (a transpose view, a strided slice) materializes first and
    the kernels keep their dense row-major fast paths. transpose/
    flatten/reshape live in tensor_views.cpp since v0.12.7 Task 12.
---*/
#include "TensorKernels.h"

#include <cstdint>
#include <cstring>
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
    if (IsContiguous(*view))
        return ArgInt(args, slot);
    const int32_t handle = Materialize(host, *view);
    view->elemKind = 0xEE;
    host->getArrayView(host, handle, view);
    return handle;
}

//The copy geometry around the concat axis: `outer` slices before it,
//`inner` contiguous cells after it per slice.
void AxisCopyGeometry(const NativeArrayView& a, int32_t axis,
                      int64_t* outer, int64_t* inner) {
    *outer = 1;
    for (int32_t d = 0; d < axis; ++d)
        *outer *= a.dims[d];
    *inner = 1;
    for (int32_t d = axis + 1; d < a.rank; ++d)
        *inner *= a.dims[d];
}

//Concat body: equal ranks, every dim equal except the axis dim (which
//sums); an out-of-range axis raises the bounds family. Row-major copy
//per (outer-slice, source) block — the cells after the axis are
//contiguous, so each block is one memcpy.
void ConcatKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView a;
    NativeArrayView b;
    ContiguousOperand(host, args, 0, &a);
    ContiguousOperand(host, args, 1, &b);
    if (a.elemKind != b.elemKind)
        Raise(host, NEXC_Base, "tensor.concat: element type mismatch");
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
    const int32_t handle = NewArrayShaped(host, a.elemKind, a.rank, outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    int64_t outer = 0;
    int64_t inner = 0;
    AxisCopyGeometry(a, axis, &outer, &inner);
    const int64_t cellBytes = a.elemCellBytes;
    const int64_t aAlong = a.dims[axis];
    const int64_t bAlong = b.dims[axis];
    const char* aCells = static_cast<const char*>(a.data);
    const char* bCells = static_cast<const char*>(b.data);
    char* outCells = static_cast<char*>(out.data);
    for (int64_t o = 0; o < outer; ++o) {
        char* dstA = outCells
            + o * (aAlong + bAlong) * inner * cellBytes;
        std::memcpy(dstA, aCells + o * aAlong * inner * cellBytes,
                    static_cast<size_t>(aAlong * inner) * cellBytes);
        std::memcpy(dstA + aAlong * inner * cellBytes,
                    bCells + o * bAlong * inner * cellBytes,
                    static_cast<size_t>(bAlong * inner) * cellBytes);
    }
    ReturnInt(ret, handle);
}

//MatMul body: naive i-k-j with the accumulator taken from the element
//type — int/long wrap around (the language-wide arithmetic rule, no
//silent widening), float/double follow IEEE. Operands arrive dense
//(ContiguousOperand in the entry).
template <typename T>
void MatMulTyped(NativeHost* host, uint8_t* ret, const NativeArrayView& a,
                 const NativeArrayView& b) {
    if (a.rank != 2 || b.rank != 2)
        Raise(host, NEXC_Base, "tensor.matmul: expects rank-2 operands");
    //The language-side overloads keep the kinds equal; the gate guards
    //the ABI surface (a mismatched pair would read b's cells through
    //a's carrier and return garbage). Same message shape as the
    //elementwise family.
    if (a.elemKind != b.elemKind)
        Raise(host, NEXC_Base, "tensor.matmul: element type mismatch");
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
    const int32_t handle = NewArrayShaped(host, a.elemKind, 2, outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const T* aCells = CellsOf<T>(a);
    const T* bCells = CellsOf<T>(b);
    T* outCells = CellsOf<T>(out);
    for (int32_t i = 0; i < aRows; ++i) {
        for (int32_t k = 0; k < aCols; ++k) {
            const T aik = aCells[static_cast<int64_t>(i) * aCols + k];
            for (int32_t j = 0; j < bCols; ++j) {
                T& acc = outCells[static_cast<int64_t>(i) * bCols + j];
                acc = static_cast<T>(
                    acc + aik * bCells[static_cast<int64_t>(k) * bCols + j]);
            }
        }
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
