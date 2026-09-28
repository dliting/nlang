// --- bytecode walk (stride table + line/pc map) ---
// Split from Disassembler.cpp (2026-09-27 maintainability refactor,
// zero behavior change): this TU owns the operand-size walk shared by
// the line/pc map and external consumers, the printing TU owns the
// instruction-line formatting.

#include "Disassembler.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace nlang {

//Read-side twin of VmBackend.cpp's compiler-side emission walk (used by
//RemapBytecode); two tables on purpose — they differ by failure policy
//(the compiler side asserts on a bug in code it just emitted; here the
//unknown-opcode arm throws so a debugger walking garbage bytecode
//reports it instead of dying). Equivalence is pinned by
//test_instruction_stride_exact_landing.
size_t InstructionStride(OpCode op) {
    switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_Array_to_str:
        case OpCode::OP_Func_to_str:
        case OpCode::OP_ParaEnd:
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            return 1;  // no operands
        case OpCode::OP_Prim_to_str:
            return 1 + 1;  // uint8 kind (0.7.5 kind-immediate)
        case OpCode::OP_PrimCast:
            return 1 + 1 + 1;  // uint8 srcKind + uint8 dstKind
        case OpCode::OP_Neg:
            return 1 + 1 + 2;  // uint8 kind + uint16 dst
        case OpCode::OP_Cmp:
            return 1 + 1 + 1 + 2 + 2;  // kind + cmpOp + two uint16 slots
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
            return 1 + 1;  // uint8 tag
        case OpCode::OP_Jump:
        case OpCode::OP_Case:
            return 1 + 2;  // int16 / uint16
        case OpCode::OP_ConstInt32:
        case OpCode::OP_ConstFloat:
            return 1 + 4;
        case OpCode::OP_ConstInt64:
        case OpCode::OP_ConstDouble:
            return 1 + 8;  // 8-byte immediate (0.7.5)
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_VarLocal:
        case OpCode::OP_Assign:
        case OpCode::OP_Enum_to_str:
        case OpCode::OP_LogicalNot:
        case OpCode::OP_Switch:
        case OpCode::OP_DebugInfo:
        case OpCode::OP_NullCheck:
        case OpCode::OP_CheckCast:
        case OpCode::OP_Throw:
        case OpCode::OP_MakeFunc:
        case OpCode::OP_MakeBoundFunc:
        case OpCode::OP_MakeVFunc:
            return 1 + 2;  // one uint16 operand
        case OpCode::OP_Add:
        case OpCode::OP_Sub:
        case OpCode::OP_Mul:
        case OpCode::OP_Div:
        case OpCode::OP_Mod:
            return 1 + 1 + 2 + 2;  // uint8 kind + two uint16 slots (0.7.5)
        case OpCode::OP_JumpIfNot:
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_Less_str:
        case OpCode::OP_LessEqual_str:
        case OpCode::OP_Greater_str:
        case OpCode::OP_GreaterEqual_str:
        case OpCode::OP_StrLen:
        case OpCode::OP_CallFunc:
        case OpCode::OP_CallMethod:
        case OpCode::OP_CallMethodDirect:
        case OpCode::OP_CallIntrinsic:
        case OpCode::OP_CallDelegate:
        case OpCode::OP_Eq_func:
        case OpCode::OP_Ne_func:
        case OpCode::OP_New:
        case OpCode::OP_ArrayLength:
            return 1 + 2 + 2;  // two uint16 operands
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut:
        case OpCode::OP_CallDelegateOut:
            return 1 + 2 + 2 + 4;  // uint16 + uint16 + uint32 outMask (Phase 9e / 13)
        case OpCode::OP_AllocStruct:
        case OpCode::OP_LoadField:
        case OpCode::OP_StoreField:
        case OpCode::OP_CopyStruct:
        case OpCode::OP_AllocArray:
        case OpCode::OP_LoadElement:
        case OpCode::OP_StoreElement:
            return 1 + 2 + 2 + 2;  // three uint16 operands
        default:
            throw std::runtime_error("unknown opcode in disassembler");
    }
}

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

} // namespace nlang
