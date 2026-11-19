/*---
    tensor_take_along.cpp — v0.14.7: takeAlongAxis, the argsort
    consumer. Per matched axis line, out[line, k] = src[line,
    indices[line, k]]; the indices share the source's shape (numpy's
    take_along_axis without the broadcast allowance). The dispatcher
    reads the indices once into a dense int32 mirror that feeds the
    bounds check, the CPU walk and the device scratch alike (one
    strided-aware read — the staged view may be dense, a host view may
    not). Out of range raises the gather discipline's diagnostic
    (index and range named; no wrap). Async: no cell errors — the
    device kernel is a pure strided copy.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

#include <cstdint>
#include <string>
#include <vector>

using nlang::native::ArgArray;
using nlang::native::ArgInt;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The per-line addressing both arms share: out is a dense fresh mint,
//so the output ordinal IS the row-major position — decompose it over
//the dims (the axis dim rides the output's dense stride), take the
//axis cell from the dense mirror.
template <typename T>
void TakeAlongCpuWalk(const NativeArrayView& va, int32_t axis,
                      const int32_t* indices, NativeArrayView& out)
{
    T* dstCells = static_cast<T*>(out.data);
    const T* srcCells = static_cast<const T*>(va.data);
    for (int64_t cell = 0; cell < out.elemCount; ++cell) {
        int64_t rem = cell;
        int64_t srcAddr = va.offset;
        for (int32_t d = out.rank - 1; d >= 0; --d) {
            const int64_t dim = out.dims[d];
            const int64_t coord = dim > 0 ? rem % dim : 0;
            rem /= (dim > 0 ? dim : 1);
            if (d != axis)
                srcAddr += coord * va.strides[d];
        }
        dstCells[out.offset + cell] =
            srcCells[srcAddr
                + static_cast<int64_t>(indices[cell])
                    * va.strides[axis]];
    }
}

//The validation and the dense mirror: the axis family's guards, the
//indices shape contract, and the bounds check over one strided-aware
//read (the staged view may be dense, a host view may not).
std::vector<int32_t> TakeAlongValidate(NativeHost* host,
                                       const NativeArrayView& va,
                                       int32_t axis,
                                       const NativeArrayView& vIdx)
{
    const char* who = "tensor.takeAlongAxis";
    if (vIdx.elemKind != NEK_Int32)
        Raise(host, NEXC_Base, std::string(who)
            + ": indices must be int (argsort's output kind)");
    if (vIdx.rank != va.rank)
        Raise(host, NEXC_Base, std::string(who)
            + ": the indices' rank must match the source's ("
            + std::to_string(va.rank) + ")");
    for (int32_t d = 0; d < va.rank; ++d)
        if (vIdx.dims[d] != va.dims[d])
            Raise(host, NEXC_Base, std::string(who)
                + ": the indices' shape must match the source's");
    const int64_t along = va.dims[axis];
    //A zero INDICES AXIS DIM makes the line count divide by zero
    //(gather's shape); a zero-size indices array with a nonzero axis
    //dim takes an empty result.
    if (vIdx.dims[axis] == 0)
        Raise(host, NEXC_Base, std::string(who)
            + ": empty indices along the axis");
    std::vector<int32_t> idxDense(static_cast<size_t>(vIdx.elemCount));
    const int32_t* cells = static_cast<const int32_t*>(vIdx.data);
    for (int64_t cell = 0; cell < vIdx.elemCount; ++cell) {
        int64_t rem = cell;
        int64_t addr = vIdx.offset;
        for (int32_t d = vIdx.rank - 1; d >= 0; --d) {
            const int64_t dim = vIdx.dims[d];
            const int64_t coord = dim > 0 ? rem % dim : 0;
            rem /= (dim > 0 ? dim : 1);
            addr += coord * vIdx.strides[d];
        }
        const int32_t index = cells[addr];
        if (index < 0 || index >= along)
            Raise(host, NEXC_OutOfBounds,
                std::string(who) + ": index " + std::to_string(index)
                + " out of bounds for axis " + std::to_string(axis)
                + " with size " + std::to_string(along));
        idxDense[static_cast<size_t>(cell)] = index;
    }
    return idxDense;
}

//The device arm: the source rides its residency, the (validated)
//dense mirror lands H2D via the pool scratch inside the launcher.
void TakeAlongDeviceSlot(NativeHost* host, NativeArrayView& va,
                         int32_t axis, const int32_t* indices,
                         NativeArrayView& out)
{
    const TensorOps& ops = OpsForResidency(1);
    switch (va.elemKind)
    {
        case NEK_Int32:
            ops.takeAlongI32(host, va, axis, indices, out);
            break;
        case NEK_Int64:
            ops.takeAlongI64(host, va, axis, indices, out);
            break;
        case NEK_Float32:
            ops.takeAlongF32(host, va, axis, indices, out);
            break;
        default:
            ops.takeAlongF64(host, va, axis, indices, out);
            break;
    }
}

//The host arm: the CPU walk for the element kind.
void TakeAlongCpuDispatch(const NativeArrayView& va, int32_t axis,
                          const int32_t* indices, NativeArrayView& out)
{
    switch (va.elemKind)
    {
        case NEK_Int32:
            TakeAlongCpuWalk<int32_t>(va, axis, indices, out);
            break;
        case NEK_Int64:
            TakeAlongCpuWalk<int64_t>(va, axis, indices, out);
            break;
        case NEK_Float32:
            TakeAlongCpuWalk<float>(va, axis, indices, out);
            break;
        default:
            TakeAlongCpuWalk<double>(va, axis, indices, out);
            break;
    }
}

//The shared entry: validate, mint (the indices' shape), route.
void TakeAlongRun(NativeHost* host, uint8_t* ret, const uint8_t* args)
{
    NativeArrayView va;
    ArgArray(host, args, 0, &va);
    const int32_t axis = ArgInt(args, 1);
    RejectAxisOutOfRange(host, "tensor.takeAlongAxis", va, axis);
    RejectZeroSizeSource(host, "tensor.takeAlongAxis", va);
    NativeArrayView vIdx;
    ArgArray(host, args, 2, &vIdx);
    //Metadata staging (the gather C4 pattern): the indices read on
    //the host — stage a device-resident view first.
    std::vector<uint8_t> idxStage;
    vIdx = StageOperandToHost(host, vIdx, &idxStage);
    const std::vector<int32_t> idxDense =
        TakeAlongValidate(host, va, axis, vIdx);
    int32_t outDims[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < va.rank; ++d)
        outDims[d] = vIdx.dims[d];
    const int32_t handle = MintOutputLike(host, va.elemKind, va.rank,
        outDims, va);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    if (va.reserved[0] == 1)
        TakeAlongDeviceSlot(host, va, axis, idxDense.data(), out);
    else
        TakeAlongCpuDispatch(va, axis, idxDense.data(), out);
    ReturnInt(ret, handle);
}

} //namespace

void TensorTakeAlong(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc)
{
    (void)argc;
    TakeAlongRun(host, ret, args);
}

//The exported CPU walk wrappers (the CPU table's slots call them; the
//walk template is file-local here).
void TakeAlongCpuF32(const NativeArrayView& a, int32_t axis,
                     const int32_t* indices, NativeArrayView& dst)
{
    TakeAlongCpuWalk<float>(a, axis, indices, dst);
}
void TakeAlongCpuF64(const NativeArrayView& a, int32_t axis,
                     const int32_t* indices, NativeArrayView& dst)
{
    TakeAlongCpuWalk<double>(a, axis, indices, dst);
}
void TakeAlongCpuI32(const NativeArrayView& a, int32_t axis,
                     const int32_t* indices, NativeArrayView& dst)
{
    TakeAlongCpuWalk<int32_t>(a, axis, indices, dst);
}
void TakeAlongCpuI64(const NativeArrayView& a, int32_t axis,
                     const int32_t* indices, NativeArrayView& dst)
{
    TakeAlongCpuWalk<int64_t>(a, axis, indices, dst);
}

} //namespace tensor
