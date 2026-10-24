// --- bytecode walk (line/pc map) ---
// Split from Disassembler.cpp (2026-09-27 maintainability refactor):
// this TU owns the line/pc map; the printing TU owns the
// instruction-line formatting. The instruction extent lives in
// InstructionStride (BytecodeOps.h) — the single table shared with the
// .ncu linker remap.

#include "Disassembler.h"
#include <cstdint>
#include <vector>

namespace nlang {

std::vector<LinePcEntry> BuildLinePcMap(const CompiledFunction& func) {
    std::vector<LinePcEntry> map;
    size_t pc = 0;
    const auto& bc = func.bytecode;
    while (pc < bc.size()) {
        OpCode op = static_cast<OpCode>(bc[pc]);
        if (op == OpCode::OP_DebugInfo && pc + 2 < bc.size()) {
            uint16_t line = static_cast<uint16_t>(
                bc[pc + 1] | (bc[pc + 2] << 8));
            //Every statement anchor is an entry (gdb-style one line,
            //multiple locations) — no same-line dedup: a collapsed copy
            //would drop real execution paths (finally normal path,
            //if/else arms) from breakpoint addressing.
            map.push_back({line, static_cast<uint16_t>(pc)});
        }
        pc += InstructionStride(op);
    }
    return map;
}

bool ModuleHasDebugAnchors(const CompiledModule& module) {
    for (const CompiledFunction& func : module.functions) {
        if (func.bytecode.empty())
            continue;   //native/stub — never carries anchors
        if (!BuildLinePcMap(func).empty())
            return true;
    }
    return false;
}

} // namespace nlang
