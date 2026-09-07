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

//Bytecode offset advance per opcode — read-side twin of VmBackend's
//compiler-side emission walk. Two tables on purpose: they differ by
//failure policy (the compiler copy asserts on an internal-consistency
//bug in code it just emitted; this one throws so a debugger walking a
//garbage module reports the error instead of dying). Equivalence is
//pinned by test_instruction_stride_exact_landing.
size_t InstructionStride(OpCode op);

//line -> pc(s) of its statement markers. Built by walking bytecode
//with InstructionStride and reading OP_DebugInfo operands directly
//(parsing disassembly text would be the wrong direction). Entries
//ascend by pc; CONSECUTIVE same-line anchors collapse to the first
//(multi-declarator lines, one-line if/else, while cond+body). A line
//may still appear more than once NON-adjacently: try/finally emits an
//exception-path copy of each finally-body line (handler region, first)
//and a normal-path copy — both real executions, so consumers picking
//breakpoint addresses must handle multiple entries per line. (A
//single-statement finally body's two copies are consecutive and
//collapse to the exception-path entry.) Also the `l`/`x` anchor table.
struct LinePcEntry {
    uint16_t line = 0;
    //u16 pc on purpose: jump-address width (Phase 9d executor precedent
    //— jump-bearing functions stay well below 64 KiB), not an oversight
    //next to DisasmLine's u32 pc.
    uint16_t pc = 0;
};
std::vector<LinePcEntry> BuildLinePcMap(const CompiledFunction& func);

} // namespace nlang
