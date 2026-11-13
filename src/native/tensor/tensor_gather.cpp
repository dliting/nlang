/*---
    tensor_gather.cpp — v0.13.7 phase 2: the index/expression surface
    that removes the last per-element loops from user code.

    gather(a, axis, indices) — uniform take-per-position: one index per
    non-axis element (indices length == elemCount / along);
    indices[flat] is the axis position for that flat non-axis position.
    Out = dims minus axis. Works on views (addressing via the recorded
    strides). Indices out of range raise IndexOutOfBounds.

    argmax/argmin(a, axis) — the same out shape, int32 indices along
    the axis, first-max tie (mirrors the scalar reduction tie rule).

    eq(a, b) — broadcast elementwise equality, int32 0/1 out, same-kind
    pairs (IEEE ==/!= for floats: NaN compares unequal).

    oneHot(labels, n) — float[len, n], row i ones at labels[i]; label
    out of range raises IndexOutOfBounds.

    All four walk scalar loops: the data they carry in real programs
    (batch-sized index/label arrays) makes SIMD marginal, and the
    expressiveness win — killing per-element user loops — is the
    point. The heavy math stays in the SIMD kernels.
---*/
#include "TensorKernels.h"

#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//Read a rank-1 int32 index array into a vector (dense read over its
//own strides).
std::vector<int32_t> ReadIndexArray(NativeHost* host, const uint8_t* args,
                                    int slot, const char* who)
{
    NativeArrayView v;
    ArgArray(host, args, slot, &v);
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        Raise(host, NEXC_Base, std::string(who)
            + ": expects a rank-1 int index array");
    std::vector<int32_t> out;
    out.reserve(static_cast<size_t>(v.elemCount));
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = v.offset;
    for (int64_t i = 0; i < v.elemCount; ++i)
    {
        out.push_back(static_cast<const int32_t*>(v.data)[addr]);
        StrideWalkAdvance(v, idx, &addr);
    }
    return out;
}

//A gather-shaped visit: walks the out flat order (dims minus axis),
//hands each visit the source address of the non-axis position, the
//flat non-axis position, and the axis index for that position
//(indices[flat]). Works on any view (addressing via the recorded
//strides).
template <typename Fn>
void VisitGather(const NativeArrayView& a, int32_t axis,
                 const std::vector<int32_t>& indices, Fn fn)
{
    const int32_t rank = a.rank;
    const int32_t along = a.dims[axis];
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = a.offset;
    int64_t flat = 0;
    for (int64_t f = 0; f < a.elemCount / along; ++f)
    {
        fn(addr, flat, indices[static_cast<size_t>(flat)]);
        //Advance the non-axis odometer: innermost first, skipping the
        //axis dim (the gather drives it from indices).
        for (int32_t d = rank - 1; d >= 0; --d)
        {
            if (d == axis)
                continue;
            idx[d] += 1;
            addr += a.strides[d];
            if (idx[d] < a.dims[d])
                break;
            addr -= static_cast<int64_t>(a.dims[d]) * a.strides[d];
            idx[d] = 0;
        }
        flat += 1;
    }
}

int32_t ElemKindOfArg(NativeHost* host, const uint8_t* args, int slot)
{
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, slot), &v);
    return v.elemKind;
}

} //namespace

//gather(a, axis, indices): out = a minus the axis dim. Uniform
//take-per-position rule: one index per non-axis element
//(indices length == elemCount / along); indices[flat] is the axis
//position for that flat non-axis position. Rank-1 sources degenerate
//to plain fancy indexing (out[i] = a[indices[i]], out length =
//indices length): removing the only axis leaves no positions to
//walk, so the indices drive the axis directly (the numpy a[idx]
//surface).
template <typename T>
void GatherKernel(NativeHost* host, uint8_t* ret, const uint8_t* args)
{
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const int32_t axis = ArgInt(args, 1);
    if (axis < 0 || axis >= va.rank)
        Raise(host, NEXC_Base, "tensor.gather: axis "
              + std::to_string(axis) + " out of range for rank "
              + std::to_string(va.rank));
    auto indices = ReadIndexArray(host, args, 2, "tensor.gather");
    const int32_t along = va.dims[axis];
    const T* cells = static_cast<const T*>(va.data);

    int32_t outDims[nlang::kMaxArrayRank];
    int32_t outRank = va.rank - 1;
    if (va.rank == 1)
    {
        //Rank-1 fancy indexing: out length = indices length.
        outRank = 1;
        outDims[0] = static_cast<int32_t>(indices.size());
    }
    else
    {
        const int64_t nonAxisCount = va.elemCount / along;
        if (static_cast<int64_t>(indices.size()) != nonAxisCount)
            Raise(host, NEXC_Base, "tensor.gather: index count "
                  + std::to_string(indices.size()) + " does not match the "
                  + std::to_string(nonAxisCount) + " non-axis positions");
        for (int32_t d = 0, o = 0; d < va.rank; ++d)
        {
            if (d == axis)
                continue;
            outDims[o++] = va.dims[d];
        }
    }
    const int32_t handle = NewArrayShaped(host, va.elemKind, outRank,
                                          outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);

    T* outCells = static_cast<T*>(out.data);
    if (va.rank == 1)
    {
        for (size_t i = 0; i < indices.size(); ++i)
        {
            const int32_t index = indices[i];
            if (index < 0 || index >= along)
                Raise(host, NEXC_OutOfBounds,
                      "tensor.gather: index " + std::to_string(index)
                      + " out of bounds for axis 0 with size "
                      + std::to_string(along));
            outCells[i] = cells[va.offset + index * va.strides[0]];
        }
        ReturnInt(ret, handle);
        return;
    }
    VisitGather(va, axis, indices,
                [&](int64_t addr, int64_t flat, int32_t index) {
        if (index < 0 || index >= along)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.gather: index " + std::to_string(index)
                  + " out of bounds for axis " + std::to_string(axis)
                  + " with size " + std::to_string(along));
        *outCells++ = cells[addr + index * va.strides[axis]];
    });
    ReturnInt(ret, handle);
}

//argmax/argmin(a, axis): int32 index array over the non-axis dims,
//first-max/min tie along the scan.
template <typename T>
void ExtremeAxisIndicesKernel(NativeHost* host, uint8_t* ret,
                              const uint8_t* args, bool isMax)
{
    const char* who = isMax ? "tensor.argmax" : "tensor.argmin";
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const int32_t axis = ArgInt(args, 1);
    if (axis < 0 || axis >= va.rank)
        Raise(host, NEXC_Base, std::string(who) + ": axis "
              + std::to_string(axis) + " out of range for rank "
              + std::to_string(va.rank));
    if (va.elemCount == 0)
        Raise(host, NEXC_Base, std::string(who) + ": empty array");

    const int32_t along = va.dims[axis];
    const int64_t outer = va.elemCount / along;
    int32_t outDims[nlang::kMaxArrayRank];
    for (int32_t d = 0, o = 0; d < va.rank; ++d)
    {
        if (d == axis)
            continue;
        outDims[o++] = va.dims[d];
    }
    const int32_t handle = NewArrayShaped(host, NEK_Int32, va.rank - 1,
                                          outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    int32_t* outCells = static_cast<int32_t*>(out.data);
    const T* cells = static_cast<const T*>(va.data);

    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = va.offset;
    for (int64_t f = 0; f < outer; ++f)
    {
        const int64_t base = addr;
        int32_t best = 0;
        T bestVal = cells[base];
        for (int32_t s = 1; s < along; ++s)
        {
            const T cur = cells[base + s * va.strides[axis]];
            if (isMax ? cur > bestVal : cur < bestVal)
            {
                bestVal = cur;
                best = s;
            }
        }
        outCells[f] = best;
        //Advance the non-axis odometer (innermost first, skipping the
        //axis dim), mirroring the gather visit.
        for (int32_t d = va.rank - 1; d >= 0; --d)
        {
            if (d == axis)
                continue;
            idx[d] += 1;
            addr += va.strides[d];
            if (idx[d] < va.dims[d])
                break;
            addr -= static_cast<int64_t>(va.dims[d]) * va.strides[d];
            idx[d] = 0;
        }
    }
    ReturnInt(ret, handle);
}

//eq(a, b): broadcast elementwise equality, int32 0/1 out. Same-kind
//pairs (the binary family rule); IEEE ==/!= for floats.
template <typename T>
void EqKernel(NativeHost* host, uint8_t* ret, const uint8_t* args)
{
    NativeArrayView va;
    NativeArrayView vb;
    ArgArray(host, args, 0, &va);
    ArgArray(host, args, 1, &vb);
    if (va.elemKind != vb.elemKind)
        Raise(host, NEXC_Base, "tensor.eq: element type mismatch");
    int32_t outDims[nlang::kMaxArrayRank];
    const int32_t outRank = BroadcastShapes(host, va, vb, outDims);
    const int32_t handle = NewArrayShaped(host, NEK_Int32, outRank,
                                          outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const T* ca = static_cast<const T*>(va.data);
    const T* cb = static_cast<const T*>(vb.data);
    int32_t* cout = static_cast<int32_t*>(out.data);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addA = va.offset;
    int64_t addB = vb.offset;
    int32_t sa[nlang::kMaxArrayRank];
    int32_t sb[nlang::kMaxArrayRank];
    BroadcastStrides(va, outRank, sa);
    BroadcastStrides(vb, outRank, sb);
    for (int64_t i = 0; i < out.elemCount; ++i)
    {
        cout[i] = ca[addA] == cb[addB] ? 1 : 0;
        for (int32_t d = outRank - 1; d >= 0; --d)
        {
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

//oneHot(labels, n): float[len, n], row i ones at labels[i]. The
//labels ride their recorded strides (a slice view's element zero sits
//offset elements into the owner — a direct data[i] read would take
//labels from the wrong place).
void OneHotKernel(NativeHost* host, uint8_t* ret, const uint8_t* args)
{
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        Raise(host, NEXC_Base,
              "tensor.oneHot: expects a rank-1 int label array");
    const int32_t n = ArgInt(args, 1);
    if (n <= 0)
        Raise(host, NEXC_Base, "tensor.oneHot: class count must be "
              "positive");
    const int32_t len = v.dims[0];
    const int32_t* labels = static_cast<const int32_t*>(v.data);
    int64_t addr = v.offset;
    for (int32_t i = 0; i < len; ++i)
    {
        if (labels[addr] < 0 || labels[addr] >= n)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.oneHot: label " + std::to_string(labels[addr])
                  + " out of range for " + std::to_string(n)
                  + " classes");
        addr += v.strides[0];
    }
    int32_t outDims[2] = {len, n};
    const int32_t handle = NewArrayShaped(host, NEK_Float32, 2, outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    float* outCells = static_cast<float*>(out.data);
    for (int32_t i = 0; i < len * n; ++i)
        outCells[i] = 0.0f;
    addr = v.offset;
    for (int32_t i = 0; i < len; ++i)
    {
        outCells[static_cast<int64_t>(i) * n + labels[addr]] = 1.0f;
        addr += v.strides[0];
    }
    ReturnInt(ret, handle);
}

} //namespace tensor

//Kind-dispatching entries (the registration surface).
namespace tensor {

void TensorGather(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc)
{
    (void)argc;
    switch (ElemKindOfArg(host, args, 0))
    {
        case NEK_Int32:
            GatherKernel<int32_t>(host, ret, args); break;
        case NEK_Int64:
            GatherKernel<int64_t>(host, ret, args); break;
        case NEK_Float32:
            GatherKernel<float>(host, ret, args); break;
        case NEK_Float64:
            GatherKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base,
                  "tensor.gather: unsupported element kind");
    }
}

void TensorArgMax(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc)
{
    (void)argc;
    switch (ElemKindOfArg(host, args, 0))
    {
        case NEK_Int32:
            ExtremeAxisIndicesKernel<int32_t>(host, ret, args, true);
            break;
        case NEK_Int64:
            ExtremeAxisIndicesKernel<int64_t>(host, ret, args, true);
            break;
        case NEK_Float32:
            ExtremeAxisIndicesKernel<float>(host, ret, args, true);
            break;
        case NEK_Float64:
            ExtremeAxisIndicesKernel<double>(host, ret, args, true);
            break;
        default:
            Raise(host, NEXC_Base,
                  "tensor.argmax: unsupported element kind");
    }
}

void TensorArgMin(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc)
{
    (void)argc;
    switch (ElemKindOfArg(host, args, 0))
    {
        case NEK_Int32:
            ExtremeAxisIndicesKernel<int32_t>(host, ret, args, false);
            break;
        case NEK_Int64:
            ExtremeAxisIndicesKernel<int64_t>(host, ret, args, false);
            break;
        case NEK_Float32:
            ExtremeAxisIndicesKernel<float>(host, ret, args, false);
            break;
        case NEK_Float64:
            ExtremeAxisIndicesKernel<double>(host, ret, args, false);
            break;
        default:
            Raise(host, NEXC_Base,
                  "tensor.argmin: unsupported element kind");
    }
}

void TensorEq(NativeHost* host, uint8_t* ret, const uint8_t* args,
              int argc)
{
    (void)argc;
    switch (ElemKindOfArg(host, args, 0))
    {
        case NEK_Int32:
            EqKernel<int32_t>(host, ret, args); break;
        case NEK_Int64:
            EqKernel<int64_t>(host, ret, args); break;
        case NEK_Float32:
            EqKernel<float>(host, ret, args); break;
        case NEK_Float64:
            EqKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base, "tensor.eq: unsupported element kind");
    }
}

void TensorOneHot(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc)
{
    (void)argc;
    OneHotKernel(host, ret, args);
}

} //namespace tensor
