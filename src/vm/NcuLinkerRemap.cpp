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

//Name-channel operands (string pool / function table). Other operands
//(local offsets, jump targets, intrinsic IDs, type tags, debug line
//numbers) are unit-local and do not need remapping.
static void PatchNameOperands(OpCode op, std::vector<uint8_t>& bc, size_t pos,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_CallMethod:
        case OpCode::OP_MakeVFunc:
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
        default:
            break;
    }
}

//v0.12.6 lazy-init protocol opcodes: every one leads with a class
//table index; the remaining operands (donePc jump target, kind tag)
//are function-local and stay untouched.
static void PatchClassInitOperands(OpCode op, std::vector<uint8_t>& bc,
    size_t pos, const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_ClassInitGate:
        case OpCode::OP_ClassInitDone:
        case OpCode::OP_ClassInitFail:
            PatchOperand(bc, pos + 1, maps.classes, "class", onUnmapped);
            break;
        default:
            break;
    }
}

//Type- and slot-channel operands (class/struct/array/enum/static-slot
//tables). SECOND-operand entries note the offset — OP_New family
//shapes lead with a dst frame offset.
static void PatchTypeOperands(OpCode op, std::vector<uint8_t>& bc, size_t pos,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
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
        case OpCode::OP_AssembleArrayN:
            // dst, typeIdx, ... — the table operand is the SECOND
            // uint16 (rank/elem base ride after it, no remap needed)
            PatchOperand(bc, pos + 3, maps.arrayTypes, "array type",
                         onUnmapped);
            break;
        case OpCode::OP_Enum_to_str:
            PatchOperand(bc, pos + 1, maps.enums, "enum", onUnmapped);
            break;
        case OpCode::OP_LoadStatic:
            // dst, slotIdx — the table operand is the SECOND uint16
            PatchOperand(bc, pos + 3, maps.slots, "static slot", onUnmapped);
            break;
        case OpCode::OP_StoreStatic:
            // slotIdx, src — the table operand is the FIRST uint16
            PatchOperand(bc, pos + 1, maps.slots, "static slot", onUnmapped);
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
        PatchNameOperands(op, bytecode, pos, maps, onUnmapped);
        PatchTypeOperands(op, bytecode, pos, maps, onUnmapped);
        PatchClassInitOperands(op, bytecode, pos, maps, onUnmapped);
        pos += InstructionStride(op);
    }
}

} //namespace nlang
