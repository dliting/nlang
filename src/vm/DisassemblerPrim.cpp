/*---
    DisassemblerPrim.cpp - kind-immediate family printers (0.7.5).
    The uint8 kind immediate is an RTK code; render it as the language
    keyword via the scalar registry ("add<int>", "cmp<long> eq").
---*/
#include "DisassemblerPrim.h"
#include "BytecodeReader.h"
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang {

static const char *PrimKindName(uint8_t rtk) {
    int i = ScalarPrimIndexOfRtk(rtk);
    return i < 0 ? "?" : kScalarPrims[i].name;
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
