/*---
    tensor_scatter.cpp — v0.14.2: the write twin of gather.
    `tensor.scatter(a, axis, indices, values)`: per position i,
    out[position(i)][indices[i]] = values[i] — the take-per-position
    contract mirrored from gather (indices length == the non-axis
    element count; one value per index; the out-of-range index raises
    IndexOutOfBoundsException by name, both bounds, no negative
    wrap). Value semantics: the function returns a NEW array (a copy
    of `a` with the writes); the language's fancy-index assignment
    `a[indices] = values` lowers onto the same kernel with `a`'s own
    payload as the target.

    Device routing: rank-1 sources take the device kernel directly
    (flat addressing, the indices staged H2D per LaunchGather's
    pattern); every other layout stages to the host walk and lands
    the result back (the gather R5 pattern — the axis-form device
    kernel lands with 0.14.3's take-per-position work). The device
    kernel is a SYNC-GROUP member: it writes the cell error flags
    (out-of-range index), so it keeps the clear-launch-sync-read
    drain and raises at this op.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"
#include "tensor_scatter_shared.h"

#include <cstdint>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::ArgFloat;
using nlang::native::ArgInt;
using nlang::native::ArgLong;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The rank-1 fancy arm: one write per index at the flat cell (any
//index count — the gather contract mirrored).
template <typename T>
void ScatterFancy1D(NativeHost* host, T* dst, const NativeArrayView& vIdx,
                    const ScatterValues& vals, int64_t along)
{
    const int32_t count = static_cast<int32_t>(vIdx.elemCount);
    const int32_t* idxCells = static_cast<const int32_t*>(vIdx.data);
    for (int32_t f = 0; f < count; ++f) {
        const int32_t index = idxCells[vIdx.offset + f];
        if (index < 0 || index >= along)
            RaiseScatterIndex(host, index, 0, along);
        dst[static_cast<int64_t>(index)] = ScatterValueOf<T>(vals, f);
    }
}

//The host walk: copy `a` into the out, then the per-position writes at
//the gather addressing (`addr + index * strides[axis]` — the visit
//odometer mirrors gather's, all axes). Rank-1 takes the fancy arm,
//rank-N the axis odometer.
template <typename T>
void ScatterCpuWalk(NativeHost* host, const NativeArrayView& va,
                    int32_t axis, const NativeArrayView& vIdx,
                    const ScatterValues& vals, NativeArrayView& out)
{
    const T* src = static_cast<const T*>(va.data);
    T* dst = static_cast<T*>(out.data);
    //The copy phase: same shape, the flat order.
    for (int64_t i = 0; i < out.elemCount; ++i)
        dst[i] = src[i];
    const int64_t along = va.dims[axis];
    if (va.rank == 1) {
        ScatterFancy1D<T>(host, dst, vIdx, vals, along);
        return;
    }
    const int32_t rank = va.rank;
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = out.offset;
    const int32_t* idxCells = static_cast<const int32_t*>(vIdx.data);
    for (int64_t f = 0; f < out.elemCount / along; ++f) {
        const int32_t index = idxCells[vIdx.offset + f];
        if (index < 0 || index >= along)
            RaiseScatterIndex(host, index, axis, along);
        dst[addr + static_cast<int64_t>(index) * out.strides[axis]]
            = ScatterValueOf<T>(vals, f);
        for (int32_t d = rank - 1; d >= 0; --d) {
            if (d == axis)
                continue;
            idx[d] += 1;
            addr += out.strides[d];
            if (idx[d] < out.dims[d])
                break;
            addr -= static_cast<int64_t>(out.dims[d]) * out.strides[d];
            idx[d] = 0;
        }
    }
}

//The one 4-way kind dispatch of the scatter slot family (the call
//shape is identical; T picks the arm — the int kinds carry the exact
//int64 scalar twin).
template <typename T>
void CallScatterSlot(const TensorOps& ops, NativeHost* host,
                     NativeArrayView& a, int32_t axis,
                     NativeArrayView& vIdx, NativeArrayView& values,
                     NativeArrayView& out, const ScatterValues& vals,
                     int32_t* errCell)
{
    if constexpr (std::is_same_v<T, float>) {
        ops.scatterF32(host, a, axis, vIdx, values, out, vals.scalar,
            vals.sv, errCell);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.scatterF64(host, a, axis, vIdx, values, out, vals.scalar,
            vals.sv, errCell);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.scatterI32(host, a, axis, vIdx, values, out, vals.scalar,
            vals.isv, errCell);
    } else {
        ops.scatterI64(host, a, axis, vIdx, values, out, vals.scalar,
            vals.isv, errCell);
    }
}

//The value-semantics run: validate, mint the copy target, route (the
//device rank-1 kernel, the staged walk or the CPU walk per residency
//and rank).
template <typename T>
void ScatterRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
                NativeArrayView& va, int32_t axis,
                NativeArrayView& vIdx, ScatterValues& vals)
{
    int32_t outDims[nlang::kMaxArrayRank];
    std::memcpy(outDims, va.dims,
        static_cast<size_t>(va.rank) * sizeof(int32_t));
    //Device rank-1: the kernel directly (the fancy-assignment shape).
    if (va.reserved[0] == 1 && va.rank == 1) {
        const int32_t handle = MintOutputLike(host, va.elemKind, va.rank,
            outDims, va);
        NativeArrayView out;
        host->getArrayView(host, handle, &out);
        int32_t errCell = -1;
        CallScatterSlot<T>(OpsForResidency(host, va), host, va, axis,
            vIdx, vals.values, out, vals, &errCell);
        if (errCell >= 0)
            RaiseScatterIndexFromCell(host, vIdx, errCell, axis,
                va.dims[axis]);
        ReturnInt(ret, handle);
        return;
    }
    //The CPU walk landing (the shared StagedWalkOut form): a host
    //source fills the mint directly; a device source (non-rank-1)
    //stages to host, fills the twin and lands H2D (the gather R5
    //pattern's shared form).
    std::vector<uint8_t> aStage;
    NativeArrayView srcHost = StageOperandToHost(host, va, &aStage);
    StagedWalkOut land;
    PrepareStagedWalkOut(host, va.elemKind, va.rank, outDims, va, &land);
    CallScatterSlot<T>(OpsForResidency(host, srcHost), host, srcHost,
        axis, vIdx, vals.values, land.fill, vals, nullptr);
    LandStagedWalkOut(host, &land);
    ReturnInt(ret, land.handle);
}

//The shared entry: gather the operands and dispatch. arg0=a, arg1=axis,
//arg2=indices, arg3=values (array or scalar per the native arm).
void ScatterEntry(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc, bool valuesScalar)
{
    (void)argc;
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const int32_t axis = ArgInt(args, 1);
    RejectAxisOutOfRange(host, "tensor.scatter", va, axis);
    RejectZeroSizeSource(host, "tensor.scatter", va);
    NativeArrayView vIdx;
    ArgArray(host, args, 2, &vIdx);
    ValidateScatterIndices(host, vIdx);
    //Metadata staging: the indices/values read on the host (the gather
    //C4 pattern) — a device-resident view stages first.
    std::vector<uint8_t> idxStage;
    vIdx = StageOperandToHost(host, vIdx, &idxStage);
    ValidateScatterCount(host, va, axis, vIdx);
    ScatterValues vals;
    std::vector<uint8_t> valStage;
    ReadScatterValues(host, args, 3, va, vIdx, valuesScalar, vals,
        valStage);
    switch (va.elemKind)
    {
        case NEK_Int32:
            ScatterRun<int32_t>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Int64:
            ScatterRun<int64_t>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Float32:
            ScatterRun<float>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Float64:
            ScatterRun<double>(host, ret, args, va, axis, vIdx, vals);
            break;
        default:
            Raise(host, NEXC_Base,
                "tensor.scatter: unsupported element kind");
    }
}

} //namespace

void ScatterCpuF32(NativeHost* host, const NativeArrayView& a,
                   int32_t axis, const NativeArrayView& indices,
                   const NativeArrayView& values, NativeArrayView& dst,
                   bool valuesScalar, double sv, int32_t* errorOut)
{
    ScatterValues vals{values, valuesScalar, sv};
    ScatterCpuWalk<float>(host, a, axis, indices, vals, dst);
}
void ScatterCpuF64(NativeHost* host, const NativeArrayView& a,
                   int32_t axis, const NativeArrayView& indices,
                   const NativeArrayView& values, NativeArrayView& dst,
                   bool valuesScalar, double sv, int32_t* errorOut)
{
    ScatterValues vals{values, valuesScalar, sv};
    ScatterCpuWalk<double>(host, a, axis, indices, vals, dst);
}
void ScatterCpuI32(NativeHost* host, const NativeArrayView& a,
                   int32_t axis, const NativeArrayView& indices,
                   const NativeArrayView& values, NativeArrayView& dst,
                   bool valuesScalar, int64_t isv, int32_t* errorOut)
{
    ScatterValues vals{values, valuesScalar, 0.0};
    vals.isv = isv;
    ScatterCpuWalk<int32_t>(host, a, axis, indices, vals, dst);
}
void ScatterCpuI64(NativeHost* host, const NativeArrayView& a,
                   int32_t axis, const NativeArrayView& indices,
                   const NativeArrayView& values, NativeArrayView& dst,
                   bool valuesScalar, int64_t isv, int32_t* errorOut)
{
    ScatterValues vals{values, valuesScalar, 0.0};
    vals.isv = isv;
    ScatterCpuWalk<int64_t>(host, a, axis, indices, vals, dst);
}

void TensorScatter(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc)
{
    ScatterEntry(host, ret, args, argc, false);
}

void TensorScatterScalar(NativeHost* host, uint8_t* ret,
                         const uint8_t* args, int argc)
{
    ScatterEntry(host, ret, args, argc, true);
}

//The in-place dispatch: the write target is `a`'s own payload (the
//assignment lowering; the value-semantics run above mints instead).
void ScatterIntoDispatch(NativeHost* host, NativeArrayView& va,
                         int32_t axis, NativeArrayView& vIdx,
                         ScatterValues& vals, int32_t* errCell)
{
    const TensorOps& ops = OpsForResidency(host, va);
    switch (va.elemKind)
    {
        case NEK_Int32:
            CallScatterSlot<int32_t>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Int64:
            CallScatterSlot<int64_t>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Float32:
            CallScatterSlot<float>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Float64:
            CallScatterSlot<double>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        default:
            Raise(host, NEXC_Base,
                "tensor.scatter: unsupported element kind");
    }
}

//v0.14.2: the fancy-index assignment's in-place arm — the write
//target is `a`'s OWN payload (the language's `a[indices] = values`
//lowering; the value-semantics scatter above mints, this one writes
//in place). The same gates and the same sync-group discipline.
void ScatterIntoEntry(NativeHost* host, uint8_t* ret, const uint8_t* args,
                      int argc, bool valuesScalar)
{
    (void)ret;
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    //The assignment form is (a, indices, values) — rank-1, axis 0
    //implicit (no axis argument).
    const int32_t axis = 0;
    RejectZeroSizeSource(host, "tensor.scatter", va);
    if (va.rank != 1)
        Raise(host, NEXC_Base,
            "tensor.scatter: the assignment form expects a rank-1 array "
            "(use tensor.scatter for the axis forms)");
    NativeArrayView vIdx;
    ArgArray(host, args, 1, &vIdx);
    ValidateScatterIndices(host, vIdx);
    std::vector<uint8_t> idxStage;
    vIdx = StageOperandToHost(host, vIdx, &idxStage);
    ScatterValues vals;
    std::vector<uint8_t> valStage;
    ReadScatterValues(host, args, 2, va, vIdx, valuesScalar, vals,
        valStage);
    int32_t errCell = -1;
    ScatterIntoDispatch(host, va, axis, vIdx, vals, &errCell);
    if (errCell >= 0)
        RaiseScatterIndexFromCell(host, vIdx, errCell, axis,
            va.dims[axis]);
}

void TensorScatterInto(NativeHost* host, uint8_t* ret, const uint8_t* args,
                       int argc)
{
    ScatterIntoEntry(host, ret, args, argc, false);
}

void TensorScatterIntoScalar(NativeHost* host, uint8_t* ret,
                             const uint8_t* args, int argc)
{
    ScatterIntoEntry(host, ret, args, argc, true);
}

} //namespace tensor
