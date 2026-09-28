/*---
    DisassemblerPrim.h - printing helpers for the 0.7.5 kind-immediate
    opcode families (OP_Add..OP_Mod, OP_Cmp, OP_Neg, OP_PrimCast,
    OP_Prim_to_str). Split out of Disassembler.cpp (file-size guard).
---*/
#pragma once
#include <cstdint>
#include <sstream>
#include <string>

namespace nlang {

class BytecodeReader;

//Each printer consumes its instruction's operands from the reader and
//appends one formatted line ("add<int> 12 20", "cmp<long> eq 4 8",
//"prim_cast long -> int", "prim_to_str<float>", "neg<byte> 6").
void DisasmPrimBinOp(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);
void DisasmPrimCmp(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);
void DisasmPrimCast(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);
void DisasmPrimToStr(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);
void DisasmPrimNeg(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);

} //namespace nlang
