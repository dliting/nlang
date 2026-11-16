/*---
    tensor_gather.cpp — v0.13.7 phase 2: the index/expression surface
    that removes the last per-element loops from user code.

    P1 T1.2d migration: the dispatcher keeps argument reads, validations
    (axis range, the gather-axis empty guard, index kind/rank, the
    index-count agreement, the argmax rank-1 raise), the rank-1 out
    shape and output minting; the run routes per call by residency (OpsForResidency)
    (gather/argmaxI32/eq/oneHot). uniform stays in tensor_random.cpp —
    its snapshot signature is a P2 artifact (plan M1).
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <cstdint>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

int32_t ElemKindOfArg(NativeHost* host, const uint8_t* args, int slot)
{
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, ArgInt(args, slot), &v);
    return v.elemKind;
}

//The index-array gate (lifted from ReadIndexArray's checks — the run
//reads the indices through the slot): rank-1 int32 or raise.
void ValidateIndexArray(NativeHost* host, const NativeArrayView& v,
                        const char* who)
{
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        Raise(host, NEXC_Base, std::string(who)
            + ": expects a rank-1 int index array");
}

//The out shape for a rank-N gather: dims minus the axis, gated on the
//index-count agreement (one index per non-axis position).
int32_t GatherOutShape(NativeHost* host, const NativeArrayView& va,
                       int32_t axis, int64_t indexCount,
                       int32_t* outDims)
{
    const int32_t along = va.dims[axis];
    const int64_t nonAxisCount = va.elemCount / along;
    if (indexCount != nonAxisCount)
        Raise(host, NEXC_Base, "tensor.gather: index count "
              + std::to_string(indexCount) + " does not match the "
              + std::to_string(nonAxisCount) + " non-axis positions");
    AxisRemovedDims(va, axis, outDims);
    return va.rank - 1;
}

} //namespace

//gather(a, axis, indices): validate, mint, route. Rank-1 sources
//degenerate to plain fancy indexing (out[i] = a[indices[i]], out
//length = indices length) — the dispatcher spells the out shape, the
//slot picks the walk by the source rank.
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
    //The empty guard keys on the GATHER AXIS being the zero dim — the
    //shape whose non-axis count below would divide by zero; a zero-size
    //source with a nonzero axis gathers empty (take-per-position).
    const int32_t along = va.dims[axis];
    if (along == 0)
        Raise(host, NEXC_Base, "tensor.gather: empty array");
    NativeArrayView vIdx;
    ArgArray(host, args, 2, &vIdx);
    ValidateIndexArray(host, vIdx, "tensor.gather");
    {
        const NativeArrayView operands[] = {va, vIdx};
        AssertSameResidency(host, operands, 2);
    }
    //Metadata staging (plan C4): the index count determines the out
    //shape and the slot reads the values — stage a device-resident
    //indices view to host first (memcpy under mirror, D2H under cuda).
    std::vector<uint8_t> idxStage;
    vIdx = StageOperandToHost(host, vIdx, &idxStage);
    //R5 (design 2026-11-15): gather routes through the HOST walk under
    //a device backend — the src stages dense (single-block D2H when
    //contiguous), the cpu table walks with the staged indices, the
    //result lands with one H2D. The bandwidth is src+out sized (batch
    //scale in real programs); the cuda take-per-position kernel lands
    //with the parity suite.
    std::vector<uint8_t> srcStage;
    NativeArrayView srcHost = va;
    if (ActiveDeviceBackend().device && va.reserved[0] == 1)
        srcHost = StageOperandToHost(host, va, &srcStage);
    int32_t outDims[nlang::kMaxArrayRank];
    int32_t outRank = va.rank - 1;
    if (va.rank == 1)
    {
        outRank = 1;
        outDims[0] = static_cast<int32_t>(vIdx.elemCount);
    }
    else
    {
        outRank = GatherOutShape(host, va, axis, vIdx.elemCount, outDims);
    }
    if (ActiveDeviceBackend().device && va.reserved[0] == 1)
    {
        //Staged path: cpu walk into a host twin, one H2D landing.
        const int32_t hostOut = host->newArrayShaped(host, va.elemKind,
            outRank, outDims);
        NativeArrayView hv;
        host->getArrayView(host, hostOut, &hv);
        const TensorOps& cpu = OpsForResidency(0);
        if constexpr (std::is_same_v<T, float>) {
            cpu.gatherF32(host, srcHost, axis, vIdx, hv);
        } else if constexpr (std::is_same_v<T, double>) {
            cpu.gatherF64(host, srcHost, axis, vIdx, hv);
        } else if constexpr (std::is_same_v<T, int32_t>) {
            cpu.gatherI32(host, srcHost, axis, vIdx, hv);
        } else {
            cpu.gatherI64(host, srcHost, axis, vIdx, hv);
        }
        const int32_t handle = host->newArrayResident(host, va.elemKind,
            outRank, outDims);
        NativeArrayView out;
        host->getArrayView(host, handle, &out);
        CopyHostToDevice(host, out.data, hv.data,
            static_cast<size_t>(hv.elemCount) * hv.elemCellBytes);
        ReturnInt(ret, handle);
        return;
    }
    const int32_t handle =
        MintOutputLike(host, va.elemKind, outRank, outDims, va);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(va.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.gatherF32(host, va, axis, vIdx, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.gatherI32(host, va, axis, vIdx, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.gatherF64(host, va, axis, vIdx, out);
    } else {
        ops.gatherI64(host, va, axis, vIdx, out);
    }
    ReturnInt(ret, handle);
}

//argmax/argmin(a, axis): int32 index array over the non-axis dims,
//first-max/min tie along the scan. The dispatcher keeps the axis/rank/
//empty validations and the mint.
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
    //The axis arm needs rank >= 2 (the max/min family precedent): the
    //result drops the axis dim, and rank-1 would need a scalar return
    //the Array-typed surface cannot spell.
    if (va.rank == 1)
        Raise(host, NEXC_Base, std::string(who)
              + ": rank-1 arrays have no axis arm");
    if (va.elemCount == 0)
        Raise(host, NEXC_Base, std::string(who) + ": empty array");

    const int32_t outRank = va.rank - 1;
    int32_t outDims[nlang::kMaxArrayRank];
    AxisRemovedDims(va, axis, outDims);
    const int32_t handle = MintOutputLike(host, NEK_Int32, outRank,
                                          outDims, va);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    OpsForResidency(va.reserved[0]).argmaxI32(host, isMax, va, axis,
                                               out);
    ReturnInt(ret, handle);
}

//eq(a, b): broadcast elementwise equality, int32 0/1 out. Same-kind
//pairs (the binary family rule); IEEE ==/!= for floats. The dispatcher
//keeps the kind gate, the broadcast shape and the mint.
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
    const int32_t handle = MintOutputLike(host, NEK_Int32, outRank,
                                          outDims, va);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(va.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.eqF32(host, va, vb, out);
    } else if constexpr (std::is_same_v<T, double>) {
        ops.eqF64(host, va, vb, out);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        ops.eqI32(host, va, vb, out);
    } else {
        ops.eqI64(host, va, vb, out);
    }
    ReturnInt(ret, handle);
}

//oneHot(labels, n): float[len, n], row i ones at labels[i]. The
//dispatcher keeps the rank/kind/class-count gates and the mint; the
//labels STAGE to host first under a device backend (the cuda kernel
//reads them from host memory), then the slot runs by the minted
//output's residency.
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
    std::vector<uint8_t> labelStage;
    v = StageOperandToHost(host, v, &labelStage);
    int32_t outDims[2] = {v.dims[0], n};
    const int32_t handle = MintArray(host, NEK_Float32, 2, outDims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    OpsForResidency(out.reserved[0]).oneHotI32(host, v, n, out);
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

} // namespace tensor
