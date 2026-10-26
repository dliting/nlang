/*---
tensor_matrix.cpp — matrix-family kernels of the `tensor` native
module: transpose (rank-2 mirror), flatten (any rank -> 1) and the
reshape family (any source rank -> the target rank carried by the
entry name; row-major throughout). All three are pure copies over
pinned views; output minting goes through the ABI v9 shaped host
callback (NewArrayShaped).
---*/
#include "TensorKernels.h"

#include <cstdint>
#include <cstring>
#include <string>

using nlang::native::ArgArray;
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

//Shared body of the reshape family: `targetRank` comes from the entry
//name, the source array and the shape vector are the two arguments.
void ReshapeKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int32_t targetRank) {
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    NativeArrayView shape;
    ArgArray(host, args, 1, &shape);
    if (shape.elemKind != NEK_Int32)
        Raise(host, NEXC_Base, "tensor.reshape: shape must be int[]");
    if (targetRank < 1 || targetRank > nlang::kMaxArrayRank
        || shape.elemCount != targetRank) {
        Raise(host, NEXC_Base,
              "tensor.reshape: shape length "
                  + std::to_string(shape.elemCount)
                  + " does not match target rank "
                  + std::to_string(targetRank));
    }
    //Saturating product with per-dim validation: negative dims raise
    //the sized-creation family (OOB); a final mismatch the base
    //Exception with both numbers spelled out.
    int32_t dims[nlang::kMaxArrayRank];
    int64_t product = 1;
    const int32_t* shapeCells = static_cast<const int32_t*>(shape.data);
    for (int32_t d = 0; d < targetRank; ++d) {
        const int32_t dim = shapeCells[d];
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
    if (product != src.elemCount) {
        Raise(host, NEXC_Base,
              "tensor.reshape: element count "
                  + std::to_string(src.elemCount)
                  + " does not match shape product "
                  + std::to_string(product));
    }
    const int32_t handle = NewArrayShaped(host, src.elemKind, targetRank,
                                          dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    std::memcpy(out.data, src.data,
                static_cast<size_t>(src.elemCount) * src.elemCellBytes);
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

//Reshape entries: the target rank is the trailing digit of the entry
//name, threaded into the shared kernel as a compile-time constant.
#define TENSOR_RESHAPE_DEF(Rank)                                           \
    void TensorReshape##Rank(NativeHost* host, uint8_t* ret,               \
                             const uint8_t* args, int argc) {              \
        (void)argc;                                                        \
        ReshapeKernel(host, ret, args, (Rank));                            \
    }
TENSOR_RESHAPE_DEF(1)
TENSOR_RESHAPE_DEF(2)
TENSOR_RESHAPE_DEF(3)
TENSOR_RESHAPE_DEF(4)
TENSOR_RESHAPE_DEF(5)
TENSOR_RESHAPE_DEF(6)
TENSOR_RESHAPE_DEF(7)
TENSOR_RESHAPE_DEF(8)

} // namespace tensor
