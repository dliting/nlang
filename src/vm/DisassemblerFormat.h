#pragma once
//Shared instruction-line formatters for the disassembler TUs (the
//family files print "head + operands" lines; these three helpers are
//the vocabulary they share). Owns the operand-count formatter and the
//two degenerate printers.

#include "BytecodeReader.h"
#include <sstream>
#include <string>

namespace nlang {

//Emit a line with no operands beyond the head.
void EmitPlain(std::ostringstream &out, const std::string &head);

//Read n uint16 operands and print them through fmt (a printf format
//consuming n values). n is capped by the fixed operand window (5).
void DisasmU16xN(int n, BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const char *fmt);

//Read the uint8 type tag and emit it as " typeTag=<signed>".
void EmitU8Tag(BytecodeReader &r, std::ostringstream &out,
    const std::string &head);

} // namespace nlang
