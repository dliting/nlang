/*---
    VmPrimOps.h - function-pointer dispatch tables for the generalized
    numeric opcodes. The executor never switches on kind: OP_Add reads
    the kind immediate and calls kAddTable[row]. Instantiated from the
    scalar registry, so a future fp16 row lights up everywhere.
---*/
#pragma once
#include "nlang/runtime/PrimitiveTypes.h"
#include <cstdint>

namespace nlang {

using PrimSlot = uint8_t*;              // frame slot storage (byte-addressed)
using PrimConst = const uint8_t*;

//dst = dst <op> src, both slots holding the carrier of the kind row.
using PrimBinFn = void (*)(PrimSlot dst, PrimConst src);
//dst = unary op applied to dst (negation, bitwise complement).
using PrimNegFn = void (*)(PrimSlot dst);
//0/1 result (bool slot).
using PrimCmpFn = int (*)(PrimConst lhs, PrimConst rhs);
//in-place convert src carrier -> dst carrier.
using PrimCastFn = void (*)(PrimSlot pResult);

//Index = scalar registry row id (SPR_*); runtime lookup via
//ScalarPrimIndexOfRtk(kindImmediate).
extern const PrimBinFn kAddTable[];  extern const PrimBinFn kSubTable[];
extern const PrimBinFn kMulTable[];  extern const PrimBinFn kDivTable[];
extern const PrimBinFn kModTable[];  extern const PrimNegFn kNegTable[];
//0.9.9 shifts and 0.13.7 bitwise ops: integer rows only, float/bool/
//char rows nullptr (resolver rejects those kinds at compile time).
extern const PrimBinFn kShlTable[];  extern const PrimBinFn kShrTable[];
extern const PrimBinFn kBitAndTable[]; extern const PrimBinFn kBitOrTable[];
extern const PrimBinFn kBitXorTable[]; extern const PrimNegFn kBitNotTable[];
//kCmpTable[cmpOp][row]; bool rows carry Eq/Ne only, rest nullptr.
extern const PrimCmpFn kCmpTable[6][SPR_Count];
//Cast grid accessor: bool rows/cols return nullptr (never emitted).
//Out-of-range rows (bad kind immediate) -> nullptr; the caller raises
//the named unsupported-kind error (guard lives here, callers stay
//branch-free).
PrimCastFn PrimCastCell(int srcRow, int dstRow);

} // namespace nlang
