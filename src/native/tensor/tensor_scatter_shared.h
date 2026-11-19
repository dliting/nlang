/*---
    tensor_scatter_shared.h — the scatter families' shared helpers
    (the values arm, the index gates, the named raise and the staged
    values reader): the write twin (tensor_scatter.cpp) and the
    accumulate twin (tensor_scatter_add.cpp) include this; inline
    here keeps one definition for both translation units.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include "backend_registry.h"

#include <string>
#include <type_traits>

namespace tensor {

//The values arm: an array (length == the index count) or a scalar
//(broadcast — the numpy put_along_axis rule). The flag + field carry
//the scalar; the view of a scalar arm is unused.
struct ScatterValues {
    NativeArrayView values = {};
    bool scalar = false;
    double sv = 0.0;
    int64_t isv = 0;   //the exact int64 twin (the double roundtrip
                       //would lose precision above 2^53 — the int
                       //kinds read it)
};

//The index-array gate (gather's ValidateIndexArray contract): rank-1
//int32 or raise.
inline void ValidateScatterIndices(NativeHost* host, const NativeArrayView& v)
{
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        nlang::native::Raise(host, NEXC_Base,
            "tensor.scatter: expects a rank-1 int index array");
}

//The count gate (the AXIS form only, rank >= 2: one index per non-axis
//position). Rank-1 is the fancy form — any index count writes any
//subset, the values length rides the index count (the gather contract
//mirrored).
inline void ValidateScatterCount(NativeHost* host, const NativeArrayView& va,
                          int32_t axis, const NativeArrayView& vIdx)
{
    if (va.rank < 2)
        return;
    const int64_t along = va.dims[axis];
    const int64_t nonAxisCount = va.elemCount / along;
    if (vIdx.elemCount != nonAxisCount)
        nlang::native::Raise(host, NEXC_Base, "tensor.scatter: index count "
            + std::to_string(vIdx.elemCount) + " does not match the "
            + std::to_string(nonAxisCount) + " non-axis positions");
}

//The named out-of-range raise (both bounds; no negative wrap — the
//language's subscript semantics; every raise site shares the wording).
inline void RaiseScatterIndex(NativeHost* host, int32_t index, int32_t axis,
                       int64_t along)
{
    nlang::native::Raise(host, NEXC_OutOfBounds,
        "tensor.scatter: index " + std::to_string(index)
        + " out of bounds for axis " + std::to_string(axis)
        + " with size " + std::to_string(along));
}

//The device kernel's flag drain: the offending index ordinal reads
//from the host-staged indices; the raise mirrors the CPU walk.
inline void RaiseScatterIndexFromCell(NativeHost* host,
                               const NativeArrayView& vIdx,
                               int32_t errCell, int32_t axis,
                               int64_t along)
{
    const int32_t bad = static_cast<const int32_t*>(
        vIdx.data)[vIdx.offset + errCell];
    RaiseScatterIndex(host, bad, axis, along);
}

//The per-position value: the scalar arm (the exact int64 twin for the
//int kinds — the double carrier loses precision above 2^53) or the
//values array's cell at the same ordinal.
template <typename T>
T ScatterValueOf(const ScatterValues& vals, int64_t f)
{
    return vals.scalar
        ? (std::is_integral_v<T> ? static_cast<T>(vals.isv)
                                 : static_cast<T>(vals.sv))
        : static_cast<const T*>(vals.values.data)[
            vals.values.offset + f];
}

//The values-arm reader shared by the two entries: a scalar rides the
//exact-int64 twin (the int kinds) or the double (the float kinds); an
//array value is kind-checked, length-checked against the index count
//and staged to host (the metadata pattern). The caller owns
//valStage — the staged view's data points INTO it, so it must outlive
//every read of vals.values (the gather dispatcher's buffer rule).
inline void ReadScatterValues(NativeHost* host, const uint8_t* args, int slot,
                       const NativeArrayView& va,
                       const NativeArrayView& vIdx, bool valuesScalar,
                       ScatterValues& vals,
                       std::vector<uint8_t>& valStage)
{
    vals.scalar = valuesScalar;
    if (valuesScalar) {
        switch (va.elemKind) {
            case NEK_Int32: vals.isv = nlang::native::ArgInt(args, slot); break;
            case NEK_Int64: vals.isv = nlang::native::ArgLong(args, slot); break;
            case NEK_Float32: vals.sv = nlang::native::ArgFloat(args, slot); break;
            default: vals.sv = nlang::native::ArgDouble(args, slot); break;
        }
        return;
    }
    nlang::native::ArgArray(host, args, slot, &vals.values);
    if (vals.values.elemKind != va.elemKind)
        nlang::native::Raise(host, NEXC_Base,
            "tensor.scatter: element type mismatch");
    if (vals.values.elemCount != vIdx.elemCount)
        nlang::native::Raise(host, NEXC_Base, "tensor.scatter: value count "
            + std::to_string(vals.values.elemCount)
            + " does not match the index count "
            + std::to_string(vIdx.elemCount));
    vals.values = StageOperandToHost(host, vals.values, &valStage);
}

} // namespace tensor
