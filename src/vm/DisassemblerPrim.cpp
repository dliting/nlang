/*---
    DisassemblerPrim.cpp - kind-name family (0.7.5): the kind-immediate
    instruction printers (the uint8 kind immediate is an RTK code,
    rendered as the language keyword via the scalar registry —
    "add<int>", "cmp<long> eq") and the wire-kind display names for the
    descriptor surfaces (DisasmTypeKindName: i8/u8/... style).
---*/
#include "DisassemblerPrim.h"
#include "Disassembler.h"
#include "BytecodeReader.h"
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang {

static const char *PrimKindName(uint8_t rtk) {
    int i = ScalarPrimIndexOfRtk(rtk);
    return i < 0 ? "?" : kScalarPrims[i].name;
}

//Short names for the 12 scalar registry rows, SCALAR_PRIMITIVE_DECL
//order — wire style, matching the descriptor sections' existing i32/f32
//convention (instruction kind immediates use the language keywords via
//PrimKindName above).
static const char *const kScalarShortNames[] = {
    "i32", "f32",            // int, float (legacy wire kinds 0/1)
    "i8",  "u8",             // byte, ubyte
    "i16", "u16",            // short, ushort
    "u32",                   // uint
    "i64", "u64",            // long, ulong
    "f64",                   // double
    "bool", "char"
};
static_assert(sizeof(kScalarShortNames) / sizeof(kScalarShortNames[0])
              == SPR_Count, "short-name table must mirror the registry rows");

const char *DisasmTypeKindName(uint16_t kind) {
    //Range-check before the uint8 view: a corrupt module may carry a
    //wide kind whose low byte would alias a real RTK.
    if (kind <= 0xFF) {
        int i = ScalarPrimIndexOfRtk(static_cast<uint8_t>(kind));
        if (i >= 0)
            return kScalarShortNames[i];
    }
    switch (kind) {
    case RTK_String: return "str";
    case RTK_Struct: return "struct";
    case RTK_Class:  return "class";
    case RTK_Array:  return "array";
    case RTK_Boxed:  return "boxed";
    case RTK_Func:   return "func";
    default:         return "unknown";
    }
}

//OP_Cmp's cmpOp immediate (EmitPrimOps.h PrimCmpOp order).
static const char *const kCmpOpNames[6] = {
    "less", "le", "greater", "ge", "eq", "ne"
};

void DisasmPrimBinOp(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    const char *kind = PrimKindName(r.ReadByte());
    uint16_t dst = r.ReadUint16();
    uint16_t src = r.ReadUint16();
    out << head << "<" << kind << "> " << dst << " " << src << "\n";
}

void DisasmPrimCmp(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    const char *kind = PrimKindName(r.ReadByte());
    const char *cmp = kCmpOpNames[r.ReadByte() % 6];
    uint16_t lhs = r.ReadUint16();
    uint16_t rhs = r.ReadUint16();
    out << head << "<" << kind << "> " << cmp << " " << lhs << " "
        << rhs << "\n";
}

void DisasmPrimCast(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    const char *src = PrimKindName(r.ReadByte());
    const char *dst = PrimKindName(r.ReadByte());
    out << head << " " << src << " -> " << dst << "\n";
}

void DisasmPrimToStr(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    out << head << "<" << PrimKindName(r.ReadByte()) << ">\n";
}

void DisasmPrimNeg(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    const char *kind = PrimKindName(r.ReadByte());
    uint16_t dst = r.ReadUint16();
    out << head << "<" << kind << "> " << dst << "\n";
}

} //namespace nlang
