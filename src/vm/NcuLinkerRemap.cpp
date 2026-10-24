/*---
    NcuLinkerRemap.cpp — .ncu 链接期操作数重映射游走（NcuLinkerRemap.h）。
    最初拷贝适配自 backend/Import.cpp（已随 d2 整链删除），现为该内核的
    唯一真身。指令步长来自 BytecodeOps.h 的 InstructionStride 单表；
    表操作数偏移以 BytecodeOps.h 的逐操作数注释为准：
    OP_New/OP_AllocStruct/OP_AllocArrayN 的表下标是第二个 u16（首操作数
    是 dst 帧偏移）。
---*/
#include "NcuLinkerRemap.h"
#include <cassert>

namespace nlang {
namespace {


//Patch one uint16 table operand in place; unmapped indices go to the
//link diagnostics instead of silently linking to 0.
void PatchOperand(std::vector<uint8_t>& bc, size_t off,
    const std::unordered_map<uint32_t, uint32_t>& m, const char* tableKind,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    const uint16_t oldv = static_cast<uint16_t>(bc[off])
                        | (static_cast<uint16_t>(bc[off + 1]) << 8);
    auto it = m.find(oldv);
    if (it == m.end()) {
        onUnmapped(tableKind, oldv);
        return;
    }
    const uint16_t newv = static_cast<uint16_t>(it->second);
    bc[off] = static_cast<uint8_t>(newv & 0xFF);
    bc[off + 1] = static_cast<uint8_t>((newv >> 8) & 0xFF);
}

} //namespace

//Per-opcode operand rewrite: dispatch the 11 remap-relevant operand
//kinds. Other operands (local offsets, jump targets, intrinsic IDs,
//type tags, debug line numbers) are unit-local and do not need
//remapping.
static void PatchTableOperands(OpCode op, std::vector<uint8_t>& bc, size_t pos,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_CallMethod:
            PatchOperand(bc, pos + 1, maps.strings, "string", onUnmapped);
            break;
        case OpCode::OP_CallFunc:
        case OpCode::OP_CallMethodDirect:
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut:
        case OpCode::OP_MakeFunc:
        case OpCode::OP_MakeBoundFunc:
            PatchOperand(bc, pos + 1, maps.functions, "function", onUnmapped);
            break;
        case OpCode::OP_MakeVFunc:
            PatchOperand(bc, pos + 1, maps.strings, "string", onUnmapped);
            break;
        case OpCode::OP_New:
            // dst, classIdx — the table operand is the SECOND uint16
            PatchOperand(bc, pos + 3, maps.classes, "class", onUnmapped);
            break;
        case OpCode::OP_CheckCast:
            PatchOperand(bc, pos + 1, maps.classes, "class", onUnmapped);
            break;
        case OpCode::OP_AllocStruct:
            // dst, structIdx, fieldCount — table operand is the SECOND uint16
            PatchOperand(bc, pos + 3, maps.structs, "struct", onUnmapped);
            break;
        case OpCode::OP_CopyStruct:
            // dst, src, structIdx — third operand
            PatchOperand(bc, pos + 5, maps.structs, "struct", onUnmapped);
            break;
        case OpCode::OP_AllocArrayN:
            // dst, typeIdx, rank, dimsBase — table operand is the SECOND
            // uint16 (the rank rides after it, no remap needed)
            PatchOperand(bc, pos + 3, maps.arrayTypes, "array type",
                         onUnmapped);
            break;
        case OpCode::OP_Enum_to_str:
            PatchOperand(bc, pos + 1, maps.enums, "enum", onUnmapped);
            break;
        default:
            break;
    }
}

void NcuRemapBytecodeOperands(std::vector<uint8_t>& bytecode,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    size_t pos = 0;
    while (pos < bytecode.size()) {
        OpCode op = static_cast<OpCode>(bytecode[pos]);
        PatchTableOperands(op, bytecode, pos, maps, onUnmapped);
        pos += InstructionStride(op);
    }
}

} //namespace nlang
