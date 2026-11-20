/*---
    VmExecutorOpsBit.cpp - registry-instantiated dispatch tables and
    the executor handler for the 0.13.7 bitwise opcodes
    (OP_BitAnd/BitOr/BitXor/BitNot). Same table pattern as
    VmExecutorOpsPrim.cpp: integer rows carry the row’s plain
    operator, every other row is nullptr and the resolver rejects
    those kinds at compile time, so a nullptr hit is an internal
    error. No run-time range check: bit ops are total on the
    integer rows (the bounds checks are shift- and division-specific).
---*/
#include "VmExecutor.h"
#include "VmPrimOps.h"
#include "PrimRowEntry.h"
#include <nlang/runtime/RnTypes.h>

namespace nlang {

#define BITAND_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::BitAnd,
#define BITOR_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::BitOr,
#define BITXOR_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::BitXor,
#define BITNOT_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::BitNot,

const PrimBinFn kBitAndTable[] = { SCALAR_PRIMITIVE_DECL(BITAND_ROW) };
const PrimBinFn kBitOrTable[] = { SCALAR_PRIMITIVE_DECL(BITOR_ROW) };
const PrimBinFn kBitXorTable[] = { SCALAR_PRIMITIVE_DECL(BITXOR_ROW) };
const PrimNegFn  kBitNotTable[] = { SCALAR_PRIMITIVE_DECL(BITNOT_ROW) };

static_assert(sizeof(kBitAndTable) / sizeof(kBitAndTable[0]) == SPR_Count);
static_assert(sizeof(kBitOrTable) / sizeof(kBitOrTable[0]) == SPR_Count);
static_assert(sizeof(kBitXorTable) / sizeof(kBitXorTable[0]) == SPR_Count);
static_assert(sizeof(kBitNotTable) / sizeof(kBitNotTable[0]) == SPR_Count);

//The whole family dispatches through one handler (the four opcodes
//form a single case arm in DispatchFrames): kind immediate plus slots
//like the table ops, OP_BitNot the unary row (kind, dst only). The
//unsupported-kind guard answers hand-written or corrupted modules;
//compiled code never reaches it (the resolver gates operand kinds).
void VmExecutor::OpBitOp(BytecodeReader& r, uint8_t* locals, OpCode op)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (op == OpCode::OP_BitNot)
    {
        if (i < 0 || !kBitNotTable[i])
            RaiseNlangException(m_exceptionClassIdx,
                "NLang VM: bitwise op on unsupported kind");
        kBitNotTable[i](locals + dst);
        return;
    }
    uint16_t src = r.ReadUint16();
    const PrimBinFn* table =
        op == OpCode::OP_BitAnd ? kBitAndTable
        : op == OpCode::OP_BitOr ? kBitOrTable : kBitXorTable;
    if (i < 0 || !table[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: bitwise op on unsupported kind");
    table[i](locals + dst, locals + src);
}

} // namespace nlang
