/*---
    tensor_where.cpp — v0.14.2: the mask-select primitive.
    `tensor.where(mask, x, y)`: per element, mask != 0 picks x else y
    (numpy's truthiness rule; the mask comes from the comparison
    operators' int 0/1 output). Three arms ride one slot family with
    scalar flags: both arrays, x scalar, y scalar (the relu idiom
    `where(a > 0, a, 0)` is the y-scalar arm). The three operands
    broadcast together (right-aligned, the arithmetic rules); the
    output kind is x/y's shared kind.

    The dispatcher keeps the validations (mask kind, x/y kind
    agreement, the both-scalar reject, the broadcast shape), mints via
    MintOutputLike (the mint follows the array operands' residency —
    the v0.14.2 lesson: a bare newArrayShaped lands the mask on the
    host while the device kernel writes through a host pointer), and
    routes by residency. Async discipline: where raises no cell
    errors — the device kernel enqueues without a sync.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

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

//The three operand views with the scalar arms folded in: an arm's
//scalar flag means the view is unused (the scalar value rides the
//slot's field). The shared kind of x/y decides the slot family.
struct WhereOperands {
    NativeArrayView mask = {};
    NativeArrayView x = {};
    NativeArrayView y = {};
    bool xScalar;
    bool yScalar;
    double sx;
    double sy;
    int64_t isx;
    int64_t isy;
    uint8_t kind;   //NEK_ of x/y (the shared kind)
};

//Mask kind gate: int/long only — masks come from the comparison
//operators' int output (a float mask has no producer and no clear
//truthiness story; named reject).
bool WhereMaskKindOk(uint8_t maskKind)
{
    return maskKind == NEK_Int32 || maskKind == NEK_Int64;
}

//The broadcast shape of the array operands among mask/x/y (scalar arms
//don't constrain): fold BroadcastShapes pairwise over the present
//views. Returns the rank, or -1 with the diagnostic logged.
int32_t WhereBroadcastShape(NativeHost* host, const WhereOperands& ops,
                            int32_t* outDims)
{
    int32_t rank = ops.mask.rank;
    int32_t accDims[nlang::kMaxArrayRank];
    for (int32_t d = 0; d < rank; ++d)
        accDims[d] = ops.mask.dims[d];
    const NativeArrayView* arms[2] = {nullptr, nullptr};
    if (!ops.xScalar)
        arms[0] = &ops.x;
    if (!ops.yScalar)
        arms[1] = &ops.y;
    for (int32_t arm = 0; arm < 2; ++arm) {
        if (arms[arm] == nullptr)
            continue;
        const NativeArrayView& v = *arms[arm];
        //A synthetic accumulator view over the local dims (BroadcastShapes
        //merges shapes only — the strides never read).
        NativeArrayView accSyn;
        accSyn.rank = rank;
        accSyn.dims = accDims;
        accSyn.elemCount = 1;
        for (int32_t d = 0; d < rank; ++d)
            accSyn.elemCount *= accDims[d];
        int32_t merged[nlang::kMaxArrayRank];
        const int32_t mergedRank = BroadcastShapes(host, accSyn, v, merged);
        if (mergedRank < 0)
            return -1;
        for (int32_t d = 0; d < mergedRank; ++d)
            accDims[d] = merged[d];
        rank = mergedRank;
    }
    for (int32_t d = 0; d < rank; ++d)
        outDims[d] = accDims[d];
    return rank;
}

//The per-kind runner: validates, mints, routes. The scalar values ride
//the slot fields (ArgFloat reads the low half cell — the uniform
//lesson); the kind dispatch happens in the entry.
template <typename T>
void WhereRun(NativeHost* host, uint8_t* ret, const uint8_t* args,
              WhereOperands& ops)
{
    int32_t outDims[nlang::kMaxArrayRank];
    const int32_t outRank = WhereBroadcastShape(host, ops, outDims);
    if (outRank < 0)
        return;   //BroadcastShapes logged the diagnostic.
    const int32_t handle = MintOutputLike(host, ops.kind, outRank,
        outDims, ops.xScalar ? ops.y : ops.x);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& slotOps = OpsForResidency(host,
        ops.xScalar ? ops.y : ops.x);
    const bool maskLong = ops.mask.elemKind == NEK_Int64;
    if constexpr (std::is_same_v<T, float>) {
        slotOps.whereF32(host, ops.mask, ops.x, ops.y, out, maskLong,
            ops.xScalar, ops.yScalar, ops.sx, ops.sy);
    } else if constexpr (std::is_same_v<T, double>) {
        slotOps.whereF64(host, ops.mask, ops.x, ops.y, out, maskLong,
            ops.xScalar, ops.yScalar, ops.sx, ops.sy);
    } else if constexpr (std::is_same_v<T, int32_t>) {
        slotOps.whereI32(host, ops.mask, ops.x, ops.y, out, maskLong,
            ops.xScalar, ops.yScalar, ops.isx, ops.isy);
    } else {
        slotOps.whereI64(host, ops.mask, ops.x, ops.y, out, maskLong,
            ops.xScalar, ops.yScalar, ops.isx, ops.isy);
    }
    ReturnInt(ret, handle);
}

//The x/y operand reader: the arrays fill the views (the kind mismatch
//raises; both-scalar rejected — the result would be a scalar pick, not
//an array operation), the scalars ride the exact-int64 twins (the int
//kinds) or the doubles (the float kinds); returns the anchor kind (the
//shared element kind of the array arm(s)).
uint8_t WhereReadArmOperands(NativeHost* host, const uint8_t* args,
                             bool xScalar, bool yScalar,
                             WhereOperands& ops)
{
    uint8_t anchorKind = 0xEE;
    const int xSlot = 1;
    const int ySlot = 2;
    if (!xScalar) {
        ArgArray(host, args, xSlot, &ops.x);
        anchorKind = ops.x.elemKind;
    }
    if (!yScalar) {
        ArgArray(host, args, ySlot, &ops.y);
        if (anchorKind == 0xEE)
            anchorKind = ops.y.elemKind;
        else if (ops.y.elemKind != anchorKind)
            Raise(host, NEXC_Base,
                "tensor.where: x and y must share one element kind");
    }
    if (xScalar && yScalar)
        Raise(host, NEXC_Base,
            "tensor.where: at least one of x and y must be an array");
    if (xScalar) {
        switch (anchorKind) {
            case NEK_Int32: ops.isx = ArgInt(args, xSlot); break;
            case NEK_Int64: ops.isx = ArgLong(args, xSlot); break;
            case NEK_Float32: ops.sx = ArgFloat(args, xSlot); break;
            default: ops.sx = ArgDouble(args, xSlot); break;
        }
    }
    if (yScalar) {
        switch (anchorKind) {
            case NEK_Int32: ops.isy = ArgInt(args, ySlot); break;
            case NEK_Int64: ops.isy = ArgLong(args, ySlot); break;
            case NEK_Float32: ops.sy = ArgFloat(args, ySlot); break;
            default: ops.sy = ArgDouble(args, ySlot); break;
        }
    }
    return anchorKind;
}

//Device agreement of the array operands: one launch reads the mask and
//both array arms through one device's pointers, so they must share the
//(residency, device) pair (the scalar arms carry no view).
void WhereAssertOperandDevice(NativeHost* host, const WhereOperands& ops,
                              bool xScalar, bool yScalar)
{
    NativeArrayView present[3] = {};
    int32_t n = 0;
    present[n++] = ops.mask;
    if (!xScalar)
        present[n++] = ops.x;
    if (!yScalar)
        present[n++] = ops.y;
    AssertSameDevice(host, present, static_cast<size_t>(n));
}

//The shared entry: gather the operands (x/y array-or-scalar), gate the
//kinds, dispatch. arg0=mask, arg1=x, arg2=y. xIsScalar/yIsScalar select
//the arm; the scalar reader matches the OTHER arm's array kind.
void WhereEntry(NativeHost* host, uint8_t* ret, const uint8_t* args,
                int argc, bool xScalar, bool yScalar)
{
    (void)argc;
    WhereOperands ops;
    ops.xScalar = xScalar;
    ops.yScalar = yScalar;
    ops.sx = 0.0;
    ops.sy = 0.0;
    ArgArray(host, args, 0, &ops.mask);
    if (!WhereMaskKindOk(ops.mask.elemKind))
        Raise(host, NEXC_Base,
            "tensor.where: the mask must be an int or long array "
            "(the comparison operators' output)");
    //The x/y arms: the arrays fill the views, the scalars ride the
    //twins; the anchor kind (the shared element kind) comes back.
    const uint8_t anchorKind = WhereReadArmOperands(host, args, xScalar,
        yScalar, ops);
    WhereAssertOperandDevice(host, ops, xScalar, yScalar);
    //Empty mask: the output has no cells — mint and return (no launch,
    //no scratch; the zero-block rationale of the reduction family).
    if (ops.mask.elemCount == 0) {
        int32_t outDims[nlang::kMaxArrayRank];
        const int32_t outRank = WhereBroadcastShape(host, ops, outDims);
        const int32_t handle = MintOutputLike(host, anchorKind, outRank,
            outDims, ops.xScalar ? ops.y : ops.x);
        ReturnInt(ret, handle);
        return;
    }
    ops.kind = anchorKind;
    switch (anchorKind)
    {
        case NEK_Int32:
            WhereRun<int32_t>(host, ret, args, ops); break;
        case NEK_Int64:
            WhereRun<int64_t>(host, ret, args, ops); break;
        case NEK_Float32:
            WhereRun<float>(host, ret, args, ops); break;
        case NEK_Float64:
            WhereRun<double>(host, ret, args, ops); break;
        default:
            Raise(host, NEXC_Base,
                "tensor.where: unsupported element kind");
    }
}

} //namespace

//The three arms (the ABI cannot see kinds, so each arm is its own
//native; the kind dispatch happens inside).
void TensorWhere(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 int argc)
{
    WhereEntry(host, ret, args, argc, false, false);
}

void TensorWhereYScalar(NativeHost* host, uint8_t* ret,
                        const uint8_t* args, int argc)
{
    WhereEntry(host, ret, args, argc, false, true);
}

void TensorWhereXScalar(NativeHost* host, uint8_t* ret,
                        const uint8_t* args, int argc)
{
    WhereEntry(host, ret, args, argc, true, false);
}

} //namespace tensor
