#pragma once
//The call and func-value operand families (0.13.1a split from
//Disassembler.cpp): direct static/bound calls, by-name calls, and the
//MakeFunc/MakeVFunc/func-equality shapes.

#include <nlang/core/BytecodeOps.h>
#include <nlang/core/BytecodeReader.h>
#include "nlang/core/CompiledModule.h"
#include <sstream>
#include <string>

namespace nlang {

//Operand-domain dispatch: true = the opcode belongs to the family and
//was printed; false lets the caller try the next family.
bool DisasmCallOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module);
bool DisasmFuncValueOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module);

} // namespace nlang
