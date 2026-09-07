#pragma once
#include "nlang/vm/CompiledModule.h"
#include "BytecodeOps.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//One disassembled instruction: pc = bytecode offset; text = the
//"0042: name operands" line WITHOUT indent and WITHOUT trailing
//newline (consumers add their own).
struct DisasmLine {
    uint32_t pc = 0;
    std::string text;
};

//Instruction-line printer shared by the ndisasm CLI and the ndb `x`
//command (pure functions over (func, module) — no I/O here).
std::vector<DisasmLine> DisassembleCode(const CompiledFunction& func,
    const CompiledModule& module);
std::string DisassembleTryBlocks(const CompiledFunction& func);

//Bytecode offset advance per opcode (read-side twin of VmBackend's
//compiler-side static walk; deliberately not merged — the compiler
//copy predates this and merging couples front/back ends).
size_t InstructionStride(OpCode op);

//line -> first pc of its statement marker. Built by walking bytecode
//with InstructionStride and reading OP_DebugInfo operands directly
//(parsing disassembly text would be the wrong direction). Entries
//ascend by pc; a line already present keeps its FIRST pc (breakpoint
//addressing semantics: `b LINE` stops at the first statement of the
//line). Also the `l`/`x` anchor table.
struct LinePcEntry {
    uint16_t line = 0;
    uint16_t pc = 0;
};
std::vector<LinePcEntry> BuildLinePcMap(const CompiledFunction& func);

} // namespace nlang
