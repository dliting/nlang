/*---
tensor_matrix.cpp — matrix-family kernels of the `tensor` native
module: transpose (rank-2 mirror), flatten (any rank -> 1), the
generic reshape (target shape carried by the int[] argument, with the
-1 inferred dim) and concat (axis-wise join; row-major throughout).
All are pure copies over pinned views; output minting goes through
the ABI v9 shaped host callback (NewArrayShaped).
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

//Saturation ceiling for the shape-product accumulation: above any
//legal mint (sized creation caps at 1e8 elements), so a saturated
//product never equals an in-range element count and the final compare
//stays exact without int64 overflow.
constexpr int64_t kShapeProductSaturate = 2000000000LL;

void TransposeKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    //Declarations are rank-2; a null handle raised in ArgArray already.
    if (src.rank != 2)
        Raise(host, NEXC_Base, "tensor.transpose: expects a rank-2 array");
    const int32_t rows = src.dims[0];
    const int32_t cols = src.dims[1];
    const int32_t outDims[2] = {cols, rows};
    const int32_t handle = NewArrayShaped(host, src.elemKind, 2, outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const int64_t cellBytes = src.elemCellBytes;
    const char* srcCells = static_cast<const char*>(src.data);
    char* outCells = static_cast<char*>(out.data);
    for (int32_t r = 0; r < rows; ++r) {
        for (int32_t c = 0; c < cols; ++c) {
            std::memcpy(
                outCells + (static_cast<int64_t>(c) * rows + r) * cellBytes,
                srcCells + (static_cast<int64_t>(r) * cols + c) * cellBytes,
                cellBytes);
        }
    }
    ReturnInt(ret, handle);
}

void FlattenKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    const int32_t flat = static_cast<int32_t>(src.elemCount);
    const int32_t handle = NewArrayShaped(host, src.elemKind, 1, &flat);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    std::memcpy(out.data, src.data,
                static_cast<size_t>(src.elemCount) * src.elemCellBytes);
    ReturnInt(ret, handle);
}

//Validate the shape argument and fill `dims` with it, inferring the
//single -1 dim from the source element count (the product of the rest
//must divide evenly). The product accumulates saturating — above any
//legal mint, so the final compare stays exact. Raises on every
//violated rule; the count-vs-product check lives here too, so the
//caller is free to mint straight after.
void PrepareReshapeDims(NativeHost* host, const NativeArrayView& src,
                        const NativeArrayView& shape, int32_t* dims) {
    const int32_t targetRank = shape.elemCount;
    if (targetRank < 1 || targetRank > nlang::kMaxArrayRank) {
        Raise(host, NEXC_Base,
              "tensor.reshape: shape length "
                  + std::to_string(targetRank)
                  + " is outside the supported rank range");
    }
    int64_t product = 1;
    int32_t inferSlot = -1;
    const int32_t* shapeCells = static_cast<const int32_t*>(shape.data);
    for (int32_t d = 0; d < targetRank; ++d) {
        const int32_t dim = shapeCells[d];
        if (dim == -1) {
            if (inferSlot >= 0)
                Raise(host, NEXC_Base, "tensor.reshape: at most one dim may be -1");
            inferSlot = d;
            dims[d] = 1;
            continue;
        }
        if (dim < 0)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.reshape: shape dim is negative");
        dims[d] = dim;
        if (dim == 0)
            product = 0;
        else if (product > kShapeProductSaturate / dim)
            product = kShapeProductSaturate;
        else
            product *= dim;
    }
    if (inferSlot >= 0) {
        //Division form: zero-product dims carry no inference base, and
        //a non-even split has no rank the payload can fill.
        if (product == 0 || src.elemCount % product != 0) {
            Raise(host, NEXC_Base,
                  "tensor.reshape: cannot infer the -1 dim from count "
                      + std::to_string(src.elemCount)
                      + " and product " + std::to_string(product));
        }
        dims[inferSlot] = static_cast<int32_t>(src.elemCount / product);
    } else if (product != src.elemCount) {
        Raise(host, NEXC_Base,
              "tensor.reshape: element count "
                  + std::to_string(src.elemCount)
                  + " does not match shape product "
                  + std::to_string(product));
    }
}

//Reshape body: the target shape is the second argument (its length is
//the target rank). Exactly one dim may be -1 — inferred from the
//element count over the product of the others, which must divide
//evenly. Still a row-major copy; the view form lands with the view
//tasks (v0.12.7 Step 2).
void ReshapeKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    NativeArrayView shape;
    ArgArray(host, args, 1, &shape);
    if (shape.elemKind != NEK_Int32)
        Raise(host, NEXC_Base, "tensor.reshape: shape must be int[]");
    int32_t dims[nlang::kMaxArrayRank];
    PrepareReshapeDims(host, src, shape, dims);
    const int32_t handle = NewArrayShaped(host, src.elemKind,
                                          shape.elemCount, dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    std::memcpy(out.data, src.data,
                static_cast<size_t>(src.elemCount) * src.elemCellBytes);
    ReturnInt(ret, handle);
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
    ArgArray(host, args, 0, &a);
    NativeArrayView b;
    ArgArray(host, args, 1, &b);
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
//silent widening), float/double follow IEEE.
template <typename T>
void MatMulTyped(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView a;
    ArgArray(host, args, 0, &a);
    NativeArrayView b;
    ArgArray(host, args, 1, &b);
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

void TensorTranspose(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc) {
    (void)argc;
    TransposeKernel(host, ret, args);
}

void TensorFlatten(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    FlattenKernel(host, ret, args);
}

void TensorReshape(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    ReshapeKernel(host, ret, args);
}

void TensorConcat(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc) {
    (void)argc;
    ConcatKernel(host, ret, args);
}

void TensorMatmul(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc) {
    (void)argc;
    //Kind dispatch off the first operand; a null handle raised in
    //ArgArray already.
    NativeArrayView probe;
    ArgArray(host, args, 0, &probe);
    switch (probe.elemKind) {
        case NEK_Int32:   MatMulTyped<int32_t>(host, ret, args); break;
        case NEK_Int64:   MatMulTyped<int64_t>(host, ret, args); break;
        case NEK_Float32: MatMulTyped<float>(host, ret, args); break;
        case NEK_Float64: MatMulTyped<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor.matmul: unknown element kind");
    }
}

} // namespace tensor
