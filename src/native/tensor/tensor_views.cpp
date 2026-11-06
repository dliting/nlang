/*---
    tensor_views.cpp — the view family of the `tensor` native module
    (v0.12.7 Task 12): transpose (full axis reversal and the axes
    permutation), flatten/reshape (an O(1) re-view of a contiguous
    source, a dense copy otherwise), slice (the all-axes function form)
    and broadcastTo (the read-only zero-stride view). Views mint through
    the ABI v10 newArrayView entry with strides in owner space, so they
    compose: a view over a view peels to the same owning record.
---*/
#include "TensorKernels.h"

#include <cstdint>
#include <string>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::NewArrayShaped;
using nlang::native::NewArrayView;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//Saturation ceiling for the shape-product accumulation: above any
//legal mint (sized creation caps at 1e8 elements), so a saturated
//product never equals an in-range element count and the final compare
//stays exact without int64 overflow.
constexpr int64_t kShapeProductSaturate = 2000000000LL;

//Validate the reshape shape and fill dims, inferring the single -1 dim
//from the source element count (the product of the rest must divide
//evenly). Raises on every violated rule; the count-vs-product check
//lives here so callers mint straight after.
void PrepareReshapeDims(NativeHost* host, const NativeArrayView& src,
                        const int32_t* shapeCells, int32_t shapeLen,
                        int32_t* dims) {
    if (shapeLen < 1 || shapeLen > nlang::kMaxArrayRank) {
        Raise(host, NEXC_Base,
              "tensor.reshape: shape length "
                  + std::to_string(shapeLen)
                  + " is outside the supported rank range");
    }
    int64_t product = 1;
    int32_t inferSlot = -1;
    for (int32_t d = 0; d < shapeLen; ++d) {
        const int32_t dim = shapeCells[d];
        if (dim == -1) {
            if (inferSlot >= 0)
                Raise(host, NEXC_Base,
                      "tensor.reshape: at most one dim may be -1");
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
        //Zero-product dims carry no inference base, and a non-even
        //split has no rank the payload can fill.
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

//Per-axis slice normalization for the explicit function form (both
//endpoints and the step are given): negative endpoints += len, clip
//per step direction, count toward the step. Returns the kept count and
//writes the normalized start; raises on a zero step. Same ladder as
//the language's FoldOneSliceAxis minus the defaults.
int32_t NormalizeSliceAxis(NativeHost* host, int32_t len, int32_t start,
                           int32_t stop, int32_t step, int32_t* outStart) {
    if (step == 0)
        Raise(host, NEXC_Base, "tensor.slice: step cannot be zero");
    if (step > 0) {
        if (start < 0) start += len;
        if (start < 0) start = 0;
        if (start > len) start = len;
        if (stop < 0) stop += len;
        if (stop < 0) stop = 0;
        if (stop > len) stop = len;
        *outStart = start;
        if (start >= stop)
            return 0;
        return static_cast<int32_t>((stop - start + step - 1) / step);
    }
    if (start < 0) start += len;
    if (start < -1) start = -1;
    if (start > len - 1) start = len - 1;
    if (stop < 0) stop += len;
    if (stop < -1) stop = -1;
    if (stop > len - 1) stop = len - 1;
    *outStart = start;
    if (start <= stop)
        return 0;
    return static_cast<int32_t>((start - stop - 1) / -step + 1);
}

//flatten over a non-contiguous source: a dense rank-1 copy.
int32_t FlattenMaterialized(NativeHost* host, const NativeArrayView& src) {
    const int32_t flat = static_cast<int32_t>(src.elemCount);
    const int32_t handle = NewArrayShaped(host, src.elemKind, 1, &flat);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    StrideCopy(src, out);
    return handle;
}

} // namespace

//transpose: argc 1 reverses every axis; argc 2 permutes by the axes
//list (out axis d reads source axis axes[d]). Both are pure re-views —
//dims and strides reorder, the offset and the owner stay.
void TensorTranspose(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc) {
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    int32_t dims[nlang::kMaxArrayRank];
    int32_t strides[nlang::kMaxArrayRank];
    if (argc == 1) {
        for (int32_t d = 0; d < src.rank; ++d) {
            dims[d] = src.dims[src.rank - 1 - d];
            strides[d] = src.strides[src.rank - 1 - d];
        }
    } else {
        int32_t axes[nlang::kMaxArrayRank];
        ReadIntList(host, args, 1, src.rank, "tensor.transpose", axes);
        bool seen[nlang::kMaxArrayRank] = {false};
        for (int32_t d = 0; d < src.rank; ++d) {
            const int32_t ax = axes[d];
            if (ax < 0 || ax >= src.rank || seen[ax])
                Raise(host, NEXC_Base,
                      "tensor.transpose: axes must be a permutation of"
                      " 0.." + std::to_string(src.rank - 1));
            seen[ax] = true;
            dims[d] = src.dims[ax];
            strides[d] = src.strides[ax];
        }
    }
    ReturnInt(ret, NewArrayView(host, ArgInt(args, 0), src.rank, dims,
                                strides, src.offset));
}

//flatten: a contiguous source re-views to rank 1 at the same offset
//(O(1)); anything else materializes a dense row-major copy.
void TensorFlatten(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    if (IsContiguous(src)) {
        const int32_t flat = static_cast<int32_t>(src.elemCount);
        const int32_t strides[1] = {1};
        ReturnInt(ret, NewArrayView(host, ArgInt(args, 0), 1, &flat,
                                    strides, src.offset));
        return;
    }
    ReturnInt(ret, FlattenMaterialized(host, src));
}

//reshape: a contiguous source re-views to the target shape (same
//offset, dense strides); a non-contiguous source materializes — the
//row-major walk of StrideCopy is exactly the reinterpretation. The -1
//inference and count validation ride PrepareReshapeDims.
void TensorReshape(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc) {
    (void)argc;
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    NativeArrayView shape;
    ArgArray(host, args, 1, &shape);
    int32_t shapeCells[nlang::kMaxArrayRank];
    const int32_t shapeLen =
        CollectIntCells(host, shape, "tensor.reshape", shapeCells);
    int32_t dims[nlang::kMaxArrayRank];
    PrepareReshapeDims(host, src, shapeCells, shapeLen, dims);
    if (IsContiguous(src)) {
        int32_t dense[nlang::kMaxArrayRank];
        DenseStrides(dims, shapeLen, dense);
        ReturnInt(ret, NewArrayView(host, ArgInt(args, 0), shapeLen, dims,
                                    dense, src.offset));
        return;
    }
    const int32_t handle =
        NewArrayShaped(host, src.elemKind, shapeLen, dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    StrideCopy(src, out);
    ReturnInt(ret, handle);
}

//slice(a, starts, stops, steps): the all-axes function form. Each axis
//normalizes (explicit endpoints, no defaults) and keeps its stride ×
//step in owner space — the true affine step per the view-record layout
//invariant, so the record never carries a fake broadcast marker.
void TensorSlice(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 int argc) {
    (void)argc;
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    int32_t starts[nlang::kMaxArrayRank];
    int32_t stops[nlang::kMaxArrayRank];
    int32_t steps[nlang::kMaxArrayRank];
    ReadIntList(host, args, 1, src.rank, "tensor.slice", starts);
    ReadIntList(host, args, 2, src.rank, "tensor.slice", stops);
    ReadIntList(host, args, 3, src.rank, "tensor.slice", steps);
    int32_t dims[nlang::kMaxArrayRank];
    int32_t strides[nlang::kMaxArrayRank];
    int64_t offset = src.offset;
    for (int32_t k = 0; k < src.rank; ++k) {
        int32_t start = 0;
        const int32_t count = NormalizeSliceAxis(host, src.dims[k],
                                                 starts[k], stops[k],
                                                 steps[k], &start);
        const int64_t axisStride =
            static_cast<int64_t>(src.strides[k]) * steps[k];
        if (count > 1
            && (axisStride > INT32_MAX || axisStride < INT32_MIN))
            Raise(host, NEXC_OutOfBounds,
                  "tensor.slice: view range exceeds the target array "
                  "bounds");
        //count<=1 overflow corner (huge step over a single element):
        //address-irrelevant, non-zero placeholder keeps the axis off
        //the broadcast store gate; a genuine broadcast base (source
        //stride 0) still stores its true 0.
        dims[k] = count;
        strides[k] = axisStride > INT32_MAX || axisStride < INT32_MIN
            ? 1 : static_cast<int32_t>(axisStride);
        offset += static_cast<int64_t>(start) * src.strides[k];
    }
    ReturnInt(ret, NewArrayView(host, ArgInt(args, 0), src.rank, dims,
                                strides, offset));
}

//broadcastTo(a, shape): the read-only zero-stride view. Leading axes
//the source lacks address with stride 0; a size-1 source axis
//broadcasts over any target dim the same way. Aligned axes must match
//or have source size 1.
void TensorBroadcastTo(NativeHost* host, uint8_t* ret, const uint8_t* args,
                       int argc) {
    (void)argc;
    NativeArrayView src;
    ArgArray(host, args, 0, &src);
    NativeArrayView shape;
    ArgArray(host, args, 1, &shape);
    int32_t shapeCells[nlang::kMaxArrayRank];
    const int32_t targetRank =
        CollectIntCells(host, shape, "tensor.broadcastTo", shapeCells);
    if (targetRank < src.rank)
        Raise(host, NEXC_Base, "tensor.broadcastTo: target rank "
              + std::to_string(targetRank) + " is below the source rank "
              + std::to_string(src.rank));
    int32_t strides[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < targetRank; ++d) {
        const int32_t ax = d - (targetRank - src.rank);
        if (ax < 0 || src.dims[ax] == 1) {
            strides[d] = 0;
            continue;
        }
        if (src.dims[ax] != shapeCells[d])
            Raise(host, NEXC_Base, "tensor.broadcastTo: shape "
                  + ShapeText(src) + " does not broadcast to the target");
        strides[d] = src.strides[ax];
    }
    ReturnInt(ret, NewArrayView(host, ArgInt(args, 0), targetRank,
                                shapeCells, strides, src.offset));
}

} // namespace tensor
