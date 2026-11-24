/*---
    tensor_unique.cpp — v0.14.7: unique/uniqueCounts, the dedup pair.
    unique(a) flattens row-major (numpy's default spelling) and
    returns the sorted unique values as a rank-1 array of the
    source's kind; uniqueCounts(a) returns the aligned counts (int32,
    sum(counts) == a.length()). The modern-numpy NaN rule: a NaN run
    dedupes to ONE trailing NaN (the NaN cells compare unequal under
    IEEE, but the sorted run is adjacent, so the strict-less dedupe
    collapses it).

    Residency: the walk is CPU-only by design (teaching-scale — the
    sort dominates unique's cost and is device-accelerated on its
    own); a device-resident source stages to host (the caller-owned
    buffer — the staging rule) and the rank-1 result lands through
    the shared StagedWalkOut. No TensorOps slots: the op has exactly
    one execution path, so there is nothing to dispatch per residency.
    Zero-size source yields the empty result (well-defined here — the
    dedupe divides nothing by the length; numpy-consistent).
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The flat read: every cell in row-major order (the source may be any
//rank, any strides — full odometer decomposition per cell).
template <typename T>
std::vector<T> UniqueFlatten(const NativeArrayView& va)
{
    std::vector<T> flat;
    flat.reserve(static_cast<size_t>(va.elemCount));
    const T* cells = static_cast<const T*>(va.data);
    for (int64_t cell = 0; cell < va.elemCount; ++cell) {
        int64_t rem = cell;
        int64_t addr = va.offset;
        for (int32_t d = va.rank - 1; d >= 0; --d) {
            const int64_t dim = va.dims[d];
            const int64_t coord = dim > 0 ? rem % dim : 0;
            rem /= (dim > 0 ? dim : 1);
            addr += coord * va.strides[d];
        }
        flat.push_back(cells[addr]);
    }
    return flat;
}

//The shared walk: sort a flat copy (the stable sort, NaN to the end),
//dedupe on strict advancement (a value not strictly greater than the
//previous unique extends its run — adjacent NaNs collapse), count the
//runs. Returns (values, counts).
template <typename T>
void UniqueWalk(const NativeArrayView& va, std::vector<T>& values,
                std::vector<int32_t>& counts)
{
    std::vector<T> flat = UniqueFlatten<T>(va);
    std::stable_sort(flat.begin(), flat.end(),
        [](const T& a, const T& b) { return NanLess(a, b); });
    for (const T& v : flat) {
        if (!values.empty() && !NanLess(values.back(), v))
            ++counts.back();
        else {
            values.push_back(v);
            counts.push_back(1);
        }
    }
}

//The rank-1 landing: mint the output like the source's residency and
//publish the filled vectors through the shared StagedWalkOut.
template <typename T>
int32_t UniqueLand(NativeHost* host, const NativeArrayView& va,
                   const std::vector<T>& values)
{
    const int32_t count = static_cast<int32_t>(values.size());
    StagedWalkOut land;
    PrepareStagedWalkOut(host, va.elemKind, 1, &count, va, &land);
    T* cells = static_cast<T*>(land.fill.data);
    for (int32_t i = 0; i < count; ++i)
        cells[land.fill.offset + i] = values[static_cast<size_t>(i)];
    LandStagedWalkOut(host, &land);
    return land.handle;
}

//The shared entry: walk, land. The output kind comes from the
//template (unique rides the source's kind; uniqueCounts is int32).
template <typename T>
void UniqueRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
               bool withCounts)
{
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    //Host-resident sources walk directly; device-resident sources
    //stage to host (the caller-owned buffer — the staging rule).
    std::vector<uint8_t> aStage;
    NativeArrayView srcHost = StageOperandToHost(host, va, &aStage);
    std::vector<T> values;
    std::vector<int32_t> counts;
    UniqueWalk<T>(srcHost, values, counts);
    int32_t handle;
    if (withCounts) {
        const int32_t count = static_cast<int32_t>(counts.size());
        StagedWalkOut land;
        PrepareStagedWalkOut(host, NEK_Int32, 1, &count, va, &land);
        int32_t* cells = static_cast<int32_t*>(land.fill.data);
        for (int32_t i = 0; i < count; ++i)
            cells[land.fill.offset + i] = counts[static_cast<size_t>(i)];
        LandStagedWalkOut(host, &land);
        handle = land.handle;
    } else {
        handle = UniqueLand<T>(host, va, values);
    }
    ReturnInt(ret, handle);
}

int32_t ElemKindOfUniqueArg(NativeHost* host, const uint8_t* args)
{
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v);
    return v.elemKind;
}

} //namespace

void TensorUnique(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc)
{
    (void)argc;
    switch (ElemKindOfUniqueArg(host, args))
    {
        case NEK_Int32: UniqueRun<int32_t>(host, ret, args, false); break;
        case NEK_Int64: UniqueRun<int64_t>(host, ret, args, false); break;
        case NEK_Float32: UniqueRun<float>(host, ret, args, false); break;
        case NEK_Float64: UniqueRun<double>(host, ret, args, false); break;
        default:
            Raise(host, NEXC_Base,
                "tensor.unique: unsupported element kind");
    }
}

void TensorUniqueCounts(NativeHost* host, uint8_t* ret,
                        const uint8_t* args, int argc)
{
    (void)argc;
    switch (ElemKindOfUniqueArg(host, args))
    {
        case NEK_Int32: UniqueRun<int32_t>(host, ret, args, true); break;
        case NEK_Int64: UniqueRun<int64_t>(host, ret, args, true); break;
        case NEK_Float32: UniqueRun<float>(host, ret, args, true); break;
        case NEK_Float64: UniqueRun<double>(host, ret, args, true); break;
        default:
            Raise(host, NEXC_Base,
                "tensor.uniqueCounts: unsupported element kind");
    }
}

} //namespace tensor
