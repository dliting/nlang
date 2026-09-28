/*---
    EmitPrimOps.h - single encoding point for the 0.7.5 kind-immediate
    opcodes. Every emitter funnels through these so instruction strides
    cannot drift between call sites.
---*/
#pragma once
#include "BytecodeOps.h"
#include "BytecodeEmitter.h"      // src/vm/BytecodeEmitter.h
#include "nlang/runtime/PrimitiveTypes.h"

namespace nlang {

inline void EmitBinOp(BytecodeEmitter& e, OpCode op, NodeKind kind,
                      uint16_t dst, uint16_t src)
{
    e.Emit(op); e.EmitByte(RtkOfKind(kind));
    e.EmitUint16(dst); e.EmitUint16(src);
}

//cmpOp ∈ {0 Less, 1 LessEqual, 2 Greater, 3 GreaterEqual, 4 Equal,
//5 NotEqual} — the OP_Cmp immediate domain (matches kCmpTable rows).
enum PrimCmpOp : uint8_t
{
    kCmpLess = 0, kCmpLessEqual, kCmpGreater, kCmpGreaterEqual,
    kCmpEqual, kCmpNotEqual
};

inline void EmitCmp(BytecodeEmitter& e, NodeKind kind, uint8_t cmpOp,
                    uint16_t lhs, uint16_t rhs)
{
    e.Emit(OpCode::OP_Cmp); e.EmitByte(RtkOfKind(kind)); e.EmitByte(cmpOp);
    e.EmitUint16(lhs); e.EmitUint16(rhs);
}

inline void EmitPrimCast(BytecodeEmitter& e, NodeKind s, NodeKind d)
{
    e.Emit(OpCode::OP_PrimCast);
    e.EmitByte(RtkOfKind(s)); e.EmitByte(RtkOfKind(d));
}

inline void EmitNeg(BytecodeEmitter& e, NodeKind kind, uint16_t dst)
{
    e.Emit(OpCode::OP_Neg); e.EmitByte(RtkOfKind(kind));
    e.EmitUint16(dst);
}

inline void EmitPrimToStr(BytecodeEmitter& e, NodeKind kind)
{
    e.Emit(OpCode::OP_Prim_to_str); e.EmitByte(RtkOfKind(kind));
}

//Kind normalization for binary-op emission. Two remaps, both legacy
//parity:
//1. Enum types are int32 at run time (enum≡int32 convention — an
//   enum-typed variable's EvalDataType is NK_EnumDecl; the retired
//   emitters hardcoded the i32 variants for it).
//2. Every other non-scalar comparand — class/array handles (raw heap
//   or token indexes), null literals — lives in a raw 4-byte slot and
//   the legacy equality ops compared them AS int32 (o != null, array
//   identity). The resolver rejects these kinds for arithmetic, so the
//   fallback only ever feeds comparisons. String passes through: the
//   caller keys its OP_*_str dispatch on the result.
//Without the fallback RtkOfKind would emit 0xFF and the executor would
//raise "unsupported kind" on every handle comparison.
inline NodeKind BinNumericKindOf(NodeKind k)
{
    if (k == NK_String)
        return k;
    return ScalarPrimIndexOf(k) < 0 ? NK_Int32 : k;
}

} // namespace nlang
