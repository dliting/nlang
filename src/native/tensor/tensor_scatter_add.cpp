/*---
    tensor_scatter_add.cpp — v0.14.7: the accumulate twins of the
    scatter family. scatterAdd(a, axis, indices, values) adds per
    position — out[position][indices[i]] += values[i] — value
    semantics (the mint holds a copy of the source; the adds land on
    top); scatterAddInto(a, indices, values) accumulates in place on
    a rank-1 array (the gradient-buffer spelling). Duplicate indices
    SUM — every contribution lands (PyTorch's scatter_add_
    semantics): atomicAdd on the device (the float order is
    nondeterministic at the ULP level under contention), serial adds
    on the CPU walk. SYNC-GROUP like scatter: the cell error flags
    drain at this op, the named raise mirrors the CPU walk's.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"
#include "tensor_scatter_shared.h"

#include <cstring>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The rank-1 fancy arm's accumulate twin: one add per index at the
//flat cell — duplicate indices sum by the serial order (the CPU arm
//is deterministic; the device arm's atomics order is not, ULP-level
//for floats).
template <typename T>
void ScatterAddFancy1D(NativeHost* host, T* dst,
                       const NativeArrayView& vIdx,
                       const ScatterValues& vals, int64_t along)
{
    const int32_t count = static_cast<int32_t>(vIdx.elemCount);
    const int32_t* idxCells = static_cast<const int32_t*>(vIdx.data);
    for (int32_t f = 0; f < count; ++f) {
        const int32_t index = idxCells[vIdx.offset + f];
        if (index < 0 || index >= along)
            RaiseScatterIndex(host, index, 0, along);
        dst[static_cast<int64_t>(index)] += ScatterValueOf<T>(vals, f);
    }
}

//The host walk's accumulate twin: the copy phase (skipped when out
//aliases `a` — the into form writes through the source's own
//payload), then the per-position adds at the gather addressing. The
//rank-1 flat takes the fancy arm, rank-N the axis odometer.
template <typename T>
void ScatterAddCpuWalk(NativeHost* host, const NativeArrayView& va,
                       int32_t axis, const NativeArrayView& vIdx,
                       const ScatterValues& vals, NativeArrayView& out)
{
    const T* src = static_cast<const T*>(va.data);
    T* dst = static_cast<T*>(out.data);
    if (dst != src) {
        //The copy phase: same shape, the flat order (the scatter
        //walk's phase verbatim).
        for (int64_t i = 0; i < out.elemCount; ++i)
            dst[i] = src[i];
    }
    const int64_t along = va.dims[axis];
    if (va.rank == 1) {
        ScatterAddFancy1D<T>(host, dst, vIdx, vals, along);
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
            += ScatterValueOf<T>(vals, f);
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

//The accumulate family's 4-way kind dispatch (the scatter call shape
//verbatim).
template <typename T>
void CallScatterAddSlot(const TensorOps& ops, NativeHost* host,
                        NativeArrayView& a, int32_t axis,
                        NativeArrayView& vIdx, NativeArrayView& values,
                        NativeArrayView& out, const ScatterValues& vals,
                        int32_t* errCell)
{
    if constexpr (std::is_same_v<T, float>) {
        ops.scatterAddF32(host, a, axis, vIdx, values, out, vals.scalar,
            vals.sv, errCell);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.scatterAddF64(host, a, axis, vIdx, values, out, vals.scalar,
            vals.sv, errCell);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.scatterAddI32(host, a, axis, vIdx, values, out, vals.scalar,
            vals.isv, errCell);
    } else {
        ops.scatterAddI64(host, a, axis, vIdx, values, out, vals.scalar,
            vals.isv, errCell);
    }
}


//The accumulate run: validate-mint-route with the scatterAdd slots
//(the scatter run's shape verbatim — the duplicate semantics ride the
//slots: atomicAdd on the device kernel, serial adds on the walk).
template <typename T>
void ScatterAddRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
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
        CallScatterAddSlot<T>(OpsForResidency(1), host, va, axis, vIdx,
            vals.values, out, vals, &errCell);
        if (errCell >= 0)
            RaiseScatterIndexFromCell(host, vIdx, errCell, axis,
                va.dims[axis]);
        ReturnInt(ret, handle);
        return;
    }
    //The CPU walk landing (the shared StagedWalkOut form).
    std::vector<uint8_t> aStage;
    NativeArrayView srcHost = StageOperandToHost(host, va, &aStage);
    StagedWalkOut land;
    PrepareStagedWalkOut(host, va.elemKind, va.rank, outDims, va, &land);
    CallScatterAddSlot<T>(OpsForResidency(0), host, srcHost, axis, vIdx,
        vals.values, land.fill, vals, nullptr);
    LandStagedWalkOut(host, &land);
    ReturnInt(ret, land.handle);
}

} //namespace

void ScatterAddCpuF32(NativeHost* host, const NativeArrayView& a,
                      int32_t axis, const NativeArrayView& indices,
                      const NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, double sv, int32_t* errorOut)
{
    (void)errorOut;
    ScatterValues vals{values, valuesScalar, sv};
    ScatterAddCpuWalk<float>(host, a, axis, indices, vals, dst);
}
void ScatterAddCpuF64(NativeHost* host, const NativeArrayView& a,
                      int32_t axis, const NativeArrayView& indices,
                      const NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, double sv, int32_t* errorOut)
{
    (void)errorOut;
    ScatterValues vals{values, valuesScalar, sv};
    ScatterAddCpuWalk<double>(host, a, axis, indices, vals, dst);
}
void ScatterAddCpuI32(NativeHost* host, const NativeArrayView& a,
                      int32_t axis, const NativeArrayView& indices,
                      const NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, int64_t isv, int32_t* errorOut)
{
    (void)errorOut;
    ScatterValues vals{values, valuesScalar, 0.0};
    vals.isv = isv;
    ScatterAddCpuWalk<int32_t>(host, a, axis, indices, vals, dst);
}
void ScatterAddCpuI64(NativeHost* host, const NativeArrayView& a,
                      int32_t axis, const NativeArrayView& indices,
                      const NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, int64_t isv, int32_t* errorOut)
{
    (void)errorOut;
    ScatterValues vals{values, valuesScalar, 0.0};
    vals.isv = isv;
    ScatterAddCpuWalk<int64_t>(host, a, axis, indices, vals, dst);
}


//The accumulate entry: the scatter entry's gates and staging verbatim
//(the accumulate semantics live in the slots and the walks).
void ScatterAddEntry(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc, bool valuesScalar)
{
    (void)argc;
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const int32_t axis = ArgInt(args, 1);
    RejectAxisOutOfRange(host, "tensor.scatterAdd", va, axis);
    RejectZeroSizeSource(host, "tensor.scatterAdd", va);
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
            ScatterAddRun<int32_t>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Int64:
            ScatterAddRun<int64_t>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Float32:
            ScatterAddRun<float>(host, ret, args, va, axis, vIdx, vals);
            break;
        case NEK_Float64:
            ScatterAddRun<double>(host, ret, args, va, axis, vIdx, vals);
            break;
        default:
            Raise(host, NEXC_Base,
                "tensor.scatterAdd: unsupported element kind");
    }
}

//The into form's 4-way kind dispatch (the ScatterIntoDispatch shape
//verbatim — the accumulate semantics ride the scatterAdd slots with
//dst = a).
void ScatterAddIntoDispatch(NativeHost* host, NativeArrayView& va,
                            int32_t axis, NativeArrayView& vIdx,
                            ScatterValues& vals, int32_t* errCell)
{
    const TensorOps& ops = OpsForResidency(va.reserved[0]);
    switch (va.elemKind)
    {
        case NEK_Int32:
            CallScatterAddSlot<int32_t>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Int64:
            CallScatterAddSlot<int64_t>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Float32:
            CallScatterAddSlot<float>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        case NEK_Float64:
            CallScatterAddSlot<double>(ops, host, va, axis, vIdx,
                vals.values, va, vals, errCell);
            break;
        default:
            Raise(host, NEXC_Base,
                "tensor.scatterAddInto: unsupported element kind");
    }
}

//v0.14.7: the in-place accumulation arm — the write target is `a`'s
//OWN payload (the gradient-buffer spelling; the scatterAddInto twins
//of scatterInto). The same gates and the same sync-group discipline;
//the accumulate semantics ride the scatterAdd slots with dst = a.
void ScatterAddIntoEntry(NativeHost* host, uint8_t* ret,
                         const uint8_t* args, int argc, bool valuesScalar)
{
    (void)ret;
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    //The assignment form is (a, indices, values) — rank-1, axis 0
    //implicit (no axis argument).
    const int32_t axis = 0;
    RejectZeroSizeSource(host, "tensor.scatterAddInto", va);
    if (va.rank != 1)
        Raise(host, NEXC_Base,
            "tensor.scatterAddInto: the assignment form expects a "
            "rank-1 array (use tensor.scatterAdd for the axis forms)");
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
    ScatterAddIntoDispatch(host, va, axis, vIdx, vals, &errCell);
    if (errCell >= 0)
        RaiseScatterIndexFromCell(host, vIdx, errCell, axis,
            va.dims[axis]);
}

void TensorScatterAdd(NativeHost* host, uint8_t* ret, const uint8_t* args,
                      int argc)
{
    ScatterAddEntry(host, ret, args, argc, false);
}

void TensorScatterAddInto(NativeHost* host, uint8_t* ret,
                          const uint8_t* args, int argc)
{
    ScatterAddIntoEntry(host, ret, args, argc, false);
}


} //namespace tensor
