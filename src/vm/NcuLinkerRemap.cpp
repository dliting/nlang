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

//v0.12.6 lazy-init protocol opcodes: each leads with an owner table
//index whose channel the trailing kind tag selects — kind 0 the class
//table (Gate: owner@+1, donePc@+3, kind@+5; Done/Fail: owner@+1,
//kind@+3), kind 1 the interface table (Task 7, D11). The remaining
//operands (donePc jump target) are function-local and stay untouched.
static void PatchClassInitOperands(OpCode op, std::vector<uint8_t>& bc,
    size_t pos, const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_ClassInitGate:
        case OpCode::OP_ClassInitDone:
        case OpCode::OP_ClassInitFail: {
            //The kind tag read stays INSIDE the case: the instruction
            //itself spans the operand (Gate: 7 bytes, kind@+5; Done/Fail:
            //5 bytes, kind@+3), while a pre-switch read would index past
            //every short tail instruction — a function always ends on a
            //stride-1 opcode, so the hoisted form was a Debug-only
            //vector-subscript assert on ANY linked unit.
            const size_t kindOff = (op == OpCode::OP_ClassInitGate)
                ? pos + 5 : pos + 3;
            const uint16_t kind = static_cast<uint16_t>(bc[kindOff])
                | (static_cast<uint16_t>(bc[kindOff + 1]) << 8);
            const bool isInterface = (kind == 1);
            PatchOperand(bc, pos + 1,
                isInterface ? maps.interfaces : maps.classes,
                isInterface ? "interface" : "class", onUnmapped);
            break;
        }
        default:
            break;
    }
}

//v0.13.1: a descriptor-row operand (OP_New's trailing word, the cast's
//descIdx). 0xFFFF is the no-descriptor sentinel, not a table index —
//skipped.
static void PatchDescriptorOperand(std::vector<uint8_t>& bc, size_t off,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    const uint16_t descIdx = static_cast<uint16_t>(bc[off])
        | (static_cast<uint16_t>(bc[off + 1]) << 8);
    if (descIdx != 0xFFFF)
        PatchOperand(bc, off, maps.descriptors, "descriptor",
                     onUnmapped);
}

//Static-slot channel operands (the storage-table indices of the static
//access and lazy-init families — spec §6.2/§8).
static void PatchStaticSlotOperands(OpCode op, std::vector<uint8_t>& bc,
    size_t pos, const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_LoadStatic:
            PatchOperand(bc, pos + 3, maps.slots, "static slot", onUnmapped);
            break;
        case OpCode::OP_StoreStatic:
        case OpCode::OP_SlotInitGate:
        case OpCode::OP_SlotInitDone:
        case OpCode::OP_SlotInitFail:
            PatchOperand(bc, pos + 1, maps.slots, "static slot", onUnmapped);
            break;
        default:
            break;
    }
}

//Type- and descriptor-channel operands (class/struct/array/enum tables,
//descriptor rows). SECOND-operand entries note the offset — OP_New
//family shapes lead with a dst frame offset.
static void PatchTypeOperands(OpCode op, std::vector<uint8_t>& bc, size_t pos,
    const NcuOperandMaps& maps,
    const std::function<void(const char*, uint16_t)>& onUnmapped) {
    switch (op) {
        case OpCode::OP_New:
            PatchOperand(bc, pos + 3, maps.classes, "class", onUnmapped);
            PatchDescriptorOperand(bc, pos + 5, maps, onUnmapped);
            break;
        case OpCode::OP_CheckCast:
            PatchOperand(bc, pos + 1, maps.classes, "class", onUnmapped);
            //v0.13.1: descIdx is the second word (classIdx, descIdx).
            PatchDescriptorOperand(bc, pos + 3, maps, onUnmapped);
            break;
        case OpCode::OP_CheckArrayType:
            PatchOperand(bc, pos + 5, maps.arrayTypes, "array type",
                         onUnmapped);
            break;
        case OpCode::OP_AllocStruct:
            PatchOperand(bc, pos + 3, maps.structs, "struct", onUnmapped);
            break;
        case OpCode::OP_CopyStruct:
            PatchOperand(bc, pos + 5, maps.structs, "struct", onUnmapped);
            break;
        case OpCode::OP_AllocArrayN:
        case OpCode::OP_AssembleArrayN:
            PatchOperand(bc, pos + 3, maps.arrayTypes, "array type",
                         onUnmapped);
            break;
        case OpCode::OP_MakeArrayView:
            //Operand order dst/arr/typeIdx/rank/paramsBase — the view's
            //own type row (the slice result), remapped like any other.
            PatchOperand(bc, pos + 5, maps.arrayTypes, "array type",
                         onUnmapped);
            break;
        case OpCode::OP_Enum_to_str:
            PatchOperand(bc, pos + 1, maps.enums, "enum", onUnmapped);
            break;
        //v0.13.1: typeName recipe row — dst frame offset leads, the
        //descriptor index is the second operand (pos+3). Unlike OP_New
        //there is no 0xFFFF form (a compose site always names a row).
        case OpCode::OP_TypeNameCompose:
            PatchOperand(bc, pos + 3, maps.descriptors, "descriptor",
                         onUnmapped);
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
        PatchStaticSlotOperands(op, bytecode, pos, maps, onUnmapped);
        PatchClassInitOperands(op, bytecode, pos, maps, onUnmapped);
        pos += InstructionStride(op);
    }
}

} //namespace nlang
