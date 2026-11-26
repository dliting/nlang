/*---
    tensor_sort.cpp — v0.14.6: the stable ascending sort primitives.
    `tensor.sort(a, axis)` sorts each axis line independently and
    returns a new array of the same shape; `tensor.argsort(a, axis)`
    returns the int32 permutation that would sort each line (stable —
    equal elements keep their original relative order, matching the
    first-wins rule of the argmax family). NaN sorts to the end of an
    ascending line (numpy's rule). The line addressing is the
    reduction family's odometer: each line = one non-axis position,
    and the write-back lands at the same dense coordinates of the
    output (the line's elements ride the output's dense axis stride).

    The CPU walk drives std::stable_sort per line over (value,
    original ordinal) pairs; the device kernel runs one thread per
    line with an in-thread insertion sort (teaching-scale axes —
    correctness over parallelism; the dispatcher routes oversized
    lines to the host walk). Async: no cell errors.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The per-line entry: the value plus its original ordinal (the ordinal
//makes the sort stable and feeds argsort directly).
template <typename T>
struct SortEntry {
    T value;
    int32_t ordinal;
};

//The ascending comparison with the NaN rule: a NaN cell sorts to the
//end of the line (numpy's rule); two NaN cells keep their relative
//order (the stable sort's contract). The value rule is the shared
//host-side NanLess (TensorKernels.h — unique's dedupe reads it too).
template <typename T>
bool SortLess(const SortEntry<T>& a, const SortEntry<T>& b)
{
    return NanLess(a.value, b.value);
}

//The line reader: the elements of one axis line (the non-axis base
//address plus the axis stride) land in entries with their ordinals.
template <typename T>
void SortLineGather(const NativeArrayView& va, int64_t lineAddr,
                    int32_t axis, int64_t along,
                    std::vector<SortEntry<T>>& entries)
{
    entries.clear();
    for (int64_t k = 0; k < along; ++k)
        entries.push_back({static_cast<const T*>(va.data)[
            lineAddr + k * va.strides[axis]], static_cast<int32_t>(k)});
}

//The line writer: values (or the ordinals, for argsort) land at the
//output's dense coordinates of the same line.
template <typename T>
void SortLineScatter(NativeArrayView& out, int64_t lineAddr,
                     int32_t axis, int64_t along, bool withIndices,
                     const std::vector<SortEntry<T>>& entries)
{
    if (!withIndices) {
        T* cells = static_cast<T*>(out.data);
        for (int64_t k = 0; k < along; ++k)
            cells[lineAddr + k * out.strides[axis]] = entries[k].value;
        return;
    }
    int32_t* cells = static_cast<int32_t*>(out.data);
    for (int64_t k = 0; k < along; ++k)
        cells[lineAddr + k * out.strides[axis]] = entries[k].ordinal;
}

//The per-line walk: one line per non-axis position (the odometer over
//the dims without the axis advances the source and the output line
//addresses in lockstep — the output is dense, so its line base and
//axis stride come from the precomputed dense strides).
template <typename T>
void SortCpuWalk(const NativeArrayView& va, int32_t axis,
                 bool withIndices, NativeArrayView& out)
{
    const int64_t along = va.dims[axis];
    const int32_t rank = va.rank;
    std::vector<SortEntry<T>> entries;
    entries.reserve(static_cast<size_t>(along));
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t srcAddr = va.offset;
    int64_t outAddr = out.offset;
    for (int64_t line = 0; line < out.elemCount / along; ++line) {
        SortLineGather<T>(va, srcAddr, axis, along, entries);
        std::stable_sort(entries.begin(), entries.end(),
            [](const SortEntry<T>& a, const SortEntry<T>& b) {
                return SortLess<T>(a, b);
            });
        SortLineScatter<T>(out, outAddr, axis, along, withIndices,
            entries);
        for (int32_t d = rank - 1; d >= 0; --d) {
            if (d == axis)
                continue;
            idx[d] += 1;
            srcAddr += va.strides[d];
            outAddr += out.strides[d];
            if (idx[d] < va.dims[d])
                break;
            srcAddr -= static_cast<int64_t>(va.dims[d]) * va.strides[d];
            outAddr -= static_cast<int64_t>(out.dims[d])
                * out.strides[d];
            idx[d] = 0;
        }
    }
}

//The kind dispatch both arms share: call the (sort|argsort) slot for
//the element kind on the given ops table.
template <typename T>
void SortDispatch(const TensorOps& ops, NativeHost* host,
                  NativeArrayView& a, int32_t axis, bool withIndices,
                  NativeArrayView& out)
{
    if constexpr (std::is_same_v<T, float>) {
        if (withIndices)
            ops.argsortF32(host, a, axis, out);
        else
            ops.sortF32(host, a, axis, out);
    } else if constexpr (std::is_same_v<T, double>) {
        if (withIndices)
            ops.argsortF64(host, a, axis, out);
        else
            ops.sortF64(host, a, axis, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        if (withIndices)
            ops.argsortI32(host, a, axis, out);
        else
            ops.sortI32(host, a, axis, out);
    } else {
        if (withIndices)
            ops.argsortI64(host, a, axis, out);
        else
            ops.sortI64(host, a, axis, out);
    }
}

//The device arm: both views ride their native residency's table.
template <typename T>
void SortDeviceSlot(NativeHost* host, NativeArrayView& va,
                    int32_t axis, bool withIndices, NativeArrayView& out)
{
    SortDispatch<T>(OpsForResidency(host, va), host, va, axis,
        withIndices, out);
}

//The shared entry: validate, mint, route (the sort arm mints T; the
//argsort arm mints int32).
template <typename T>
void SortRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
             bool withIndices)
{
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const std::string who = withIndices ? "tensor.argsort"
        : "tensor.sort";
    const int32_t axis = ArgInt(args, 1);
    RejectAxisOutOfRange(host, who, va, axis);
    RejectZeroSizeSource(host, who, va);
    const int64_t along = va.dims[axis];
    int32_t outDims[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < va.rank; ++d)
        outDims[d] = va.dims[d];
    const uint8_t outKind = withIndices ? NEK_Int32 : va.elemKind;
    //The device arm's line-length gate: the kernel's in-thread buffer
    //is fixed at kSortMaxLine — longer lines join the CPU walk below.
    if (va.reserved[0] == 1 && along <= TensorOps::kSortMaxLine) {
        const int32_t handle = MintOutputLike(host, outKind, va.rank,
            outDims, va);
        NativeArrayView out;
        host->getArrayView(host, handle, &out);
        SortDeviceSlot<T>(host, va, axis, withIndices, out);
        ReturnInt(ret, handle);
        return;
    }
    //The CPU walk landing (the shared StagedWalkOut form): a host
    //source fills the mint directly; a device source with oversized
    //lines stages to host (the caller-owned buffer — the staging rule),
    //fills the twin and lands H2D.
    std::vector<uint8_t> aStage;
    NativeArrayView srcHost = StageOperandToHost(host, va, &aStage);
    StagedWalkOut land;
    PrepareStagedWalkOut(host, outKind, va.rank, outDims, va, &land);
    SortDispatch<T>(OpsForResidency(host, srcHost), host, srcHost, axis,
        withIndices, land.fill);
    LandStagedWalkOut(host, &land);
    ReturnInt(ret, land.handle);
}

int32_t ElemKindOfSortArg(NativeHost* host, const uint8_t* args)
{
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, 0), &v);
    return v.elemKind;
}

} //namespace

void TensorSort(NativeHost* host, uint8_t* ret, const uint8_t* args,
                int argc)
{
    (void)argc;
    switch (ElemKindOfSortArg(host, args))
    {
        case NEK_Int32: SortRun<int32_t>(host, ret, args, false); break;
        case NEK_Int64: SortRun<int64_t>(host, ret, args, false); break;
        case NEK_Float32: SortRun<float>(host, ret, args, false); break;
        case NEK_Float64: SortRun<double>(host, ret, args, false); break;
        default:
            Raise(host, NEXC_Base,
                "tensor.sort: unsupported element kind");
    }
}

void TensorArgSort(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc)
{
    (void)argc;
    switch (ElemKindOfSortArg(host, args))
    {
        case NEK_Int32: SortRun<int32_t>(host, ret, args, true); break;
        case NEK_Int64: SortRun<int64_t>(host, ret, args, true); break;
        case NEK_Float32: SortRun<float>(host, ret, args, true); break;
        case NEK_Float64: SortRun<double>(host, ret, args, true); break;
        default:
            Raise(host, NEXC_Base,
                "tensor.argsort: unsupported element kind");
    }
}

//The exported CPU walk wrappers (the CPU table's slots call them; the
//walk template is file-local here).
void SortCpuF32(const NativeArrayView& a, int32_t axis,
                bool withIndices, NativeArrayView& dst)
{
    SortCpuWalk<float>(a, axis, withIndices, dst);
}
void SortCpuF64(const NativeArrayView& a, int32_t axis,
                bool withIndices, NativeArrayView& dst)
{
    SortCpuWalk<double>(a, axis, withIndices, dst);
}
void SortCpuI32(const NativeArrayView& a, int32_t axis,
                bool withIndices, NativeArrayView& dst)
{
    SortCpuWalk<int32_t>(a, axis, withIndices, dst);
}
void SortCpuI64(const NativeArrayView& a, int32_t axis,
                bool withIndices, NativeArrayView& dst)
{
    SortCpuWalk<int64_t>(a, axis, withIndices, dst);
}

} //namespace tensor
