/*---
    NcuLinkerRemap.cpp — 指令步长族＋操作数重映射游走（NcuLinkerRemap.h）。
    最初拷贝适配自 backend/Import.cpp（已随 d2 整链删除），现为该内核的
    唯一真身。操作数偏移以 BytecodeOps.h 的逐操作数注释为准：
    OP_New/OP_AllocStruct/OP_AllocArray 的表下标是第二个 u16（首操作数
    是 dst 帧偏移）。
---*/
#include "NcuLinkerRemap.h"
#include <cassert>

namespace nlang {
namespace {

//Stride of the no-operand opcode family; 0 when op is not in it.
size_t NoOperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_CastIntToFloat:
        case OpCode::OP_CastFloatToInt:
        case OpCode::OP_Int32_to_str:
        case OpCode::OP_Float_to_str:
        case OpCode::OP_Array_to_str:
        case OpCode::OP_Func_to_str:
        case OpCode::OP_ParaEnd:
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            return 1;  // no operands
        default:
            return 0;
    }
}

//Stride of the one-uint16-operand opcode family; 0 when op is not in it.
size_t SingleU16OperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_ConstString:
        case OpCode::OP_AssertFail:
        case OpCode::OP_VarLocal:
        case OpCode::OP_Assign:
        case OpCode::OP_Enum_to_str:
        case OpCode::OP_Neg_i32:
        case OpCode::OP_Neg_f32:
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
        default:
            return 0;
    }
}

//Stride of the two-uint16-operand opcode family; 0 when op is not in it.
size_t DoubleU16OperandStride(OpCode op) {
    switch (op) {
        case OpCode::OP_JumpIfNot:
        case OpCode::OP_Add_i32:
        case OpCode::OP_Sub_i32:
        case OpCode::OP_Mul_i32:
        case OpCode::OP_Div_i32:
        case OpCode::OP_Mod_i32:
        case OpCode::OP_Add_f32:
        case OpCode::OP_Sub_f32:
        case OpCode::OP_Mul_f32:
        case OpCode::OP_Div_f32:
        case OpCode::OP_Less_i32:
        case OpCode::OP_LessEqual_i32:
        case OpCode::OP_Greater_i32:
        case OpCode::OP_GreaterEqual_i32:
        case OpCode::OP_Equal_i32:
        case OpCode::OP_NotEqual_i32:
        case OpCode::OP_Less_f32:
        case OpCode::OP_LessEqual_f32:
        case OpCode::OP_Greater_f32:
        case OpCode::OP_GreaterEqual_f32:
        case OpCode::OP_Equal_f32:
        case OpCode::OP_NotEqual_f32:
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
        default:
            return 0;
    }
}

//Rewrite the uint16 operand at byte offset `off`. The maps are total over
//the unit's indices, so a miss is a corrupt image — reported, never
//silently left stale (a stale index can alias a wrong table entry).
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

static size_t NcuInstructionStride(OpCode op) {
    if (size_t stride = NoOperandStride(op))
        return stride;
    if (size_t stride = SingleU16OperandStride(op))
        return stride;
    if (size_t stride = DoubleU16OperandStride(op))
        return stride;
    switch (op) {
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
            return 1 + 1;  // uint8 tag
        case OpCode::OP_Jump:
        case OpCode::OP_Case:
            return 1 + 2;  // int16 / uint16
        case OpCode::OP_ConstInt32:
        case OpCode::OP_ConstFloat:
            return 1 + 4;
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut:
        case OpCode::OP_CallDelegateOut:
            return 1 + 2 + 2 + 4;  // uint16 + uint16 + uint32 outMask
        case OpCode::OP_AllocStruct:
        case OpCode::OP_LoadField:
        case OpCode::OP_StoreField:
        case OpCode::OP_CopyStruct:
        case OpCode::OP_AllocArray:
        case OpCode::OP_LoadElement:
        case OpCode::OP_StoreElement:
            return 1 + 2 + 2 + 2;  // three uint16 operands
        default:
            //Unknown opcode — should never happen. Returning 1 lets the
            //walker make progress; the module will fail at runtime.
            assert(false && "unknown opcode in NcuRemapBytecodeOperands");
            return 1;
    }
}

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
        case OpCode::OP_AllocArray:
            // dst, arrayTypeIdx, sizeSlot — table operand is the SECOND uint16
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
        pos += NcuInstructionStride(op);
    }
}

} //namespace nlang
