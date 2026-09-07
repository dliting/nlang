// --- shared disassembler (ndisasm CLI + ndb `x`) ---
// Extracted verbatim from ndisasm's DisassembleFunction; the only
// change is the sink (ostringstream per instruction instead of
// std::cout). Golden byte-for-byte comparison guards the move.

#include "Disassembler.h"
#include "BytecodeReader.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

std::vector<DisasmLine> DisassembleCode(const CompiledFunction& func,
    const CompiledModule& module) {
    std::vector<DisasmLine> lines;
    BytecodeReader reader(func.bytecode.data(), func.bytecode.size());

    while (!reader.Eof()) {
        size_t offset = reader.CurrentOffset();

        //Pad offset to 4 hex digits
        char offsetBuf[16];
        snprintf(offsetBuf, sizeof(offsetBuf), "%04zx", offset);

        std::ostringstream out;
        OpCode op = reader.ReadOp();
        const char* name = OpCodeName(op);

        switch (op) {
        //No operands
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_CastIntToFloat:
        case OpCode::OP_CastFloatToInt:
        case OpCode::OP_Int32_to_str:
        case OpCode::OP_Float_to_str:
        case OpCode::OP_ParaEnd:
            out << offsetBuf << ": " << name << "\n";
            break;

        //uint16 msg string index
        case OpCode::OP_AssertFail: {
            uint16_t idx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << idx << "\n";
            break;
        }

        //int16 target (jump)
        case OpCode::OP_Jump: {
            int16_t target = reader.ReadInt16();
            char targetBuf[16];
            snprintf(targetBuf, sizeof(targetBuf), "%04x",
                     static_cast<unsigned>(static_cast<int16_t>(target)));
            out << offsetBuf << ": " << name
                << " ->" << targetBuf << "\n";
            break;
        }

        //int16 target + uint16 local (conditional jump)
        case OpCode::OP_JumpIfNot: {
            int16_t target = reader.ReadInt16();
            uint16_t localOff = reader.ReadUint16();
            char targetBuf[16];
            snprintf(targetBuf, sizeof(targetBuf), "%04x",
                     static_cast<unsigned>(static_cast<int16_t>(target)));
            out << offsetBuf << ": " << name
                << " ->" << targetBuf << " " << localOff << "\n";
            break;
        }

        //int32 value
        case OpCode::OP_ConstInt32: {
            int32_t v = reader.ReadInt32();
            out << offsetBuf << ": " << name
                << " " << v << "\n";
            break;
        }

        //float value
        case OpCode::OP_ConstFloat: {
            float v = reader.ReadFloat();
            out << offsetBuf << ": " << name
                << " " << v << "\n";
            break;
        }

        //uint16 pool_index (string constant)
        case OpCode::OP_ConstString: {
            uint16_t idx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " [" << idx << "]";
            if (idx < module.stringConstants.size())
                out << " \"" << module.stringConstants[idx] << "\"";
            out << "\n";
            break;
        }

        //uint16 offset (variable access)
        case OpCode::OP_VarLocal: {
            uint16_t off = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << off << "\n";
            break;
        }

        //uint16 dst_offset (assign)
        case OpCode::OP_Assign: {
            uint16_t dst = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << "\n";
            break;
        }

        //uint16 dst, uint16 src (binary arithmetic)
        case OpCode::OP_Add_i32:
        case OpCode::OP_Sub_i32:
        case OpCode::OP_Mul_i32:
        case OpCode::OP_Div_i32:
        case OpCode::OP_Mod_i32:
        case OpCode::OP_Add_f32:
        case OpCode::OP_Sub_f32:
        case OpCode::OP_Mul_f32:
        case OpCode::OP_Div_f32: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << " " << src << "\n";
            break;
        }

        //uint16 dst (unary)
        case OpCode::OP_Neg_i32:
        case OpCode::OP_Neg_f32:
        case OpCode::OP_LogicalNot: {
            uint16_t dst = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << "\n";
            break;
        }

        //uint16 lhs, uint16 rhs (comparison)
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
        case OpCode::OP_NotEqual_f32: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << lhs << " " << rhs << "\n";
            break;
        }

        //uint16 func_index, uint16 call_param_base
        case OpCode::OP_CallFunc: {
            uint16_t funcIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " [" << funcIdx << "]";
            if (funcIdx < module.functions.size())
                out << " " << module.functions[funcIdx].name;
            out << " base=" << base << "\n";
            break;
        }

        //uint16 func_index, uint16 call_param_base, uint32 out_mask (Phase 9e)
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut: {
            uint16_t funcIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            uint32_t outMask = reader.ReadUint32();
            out << offsetBuf << ": " << name
                << " [" << funcIdx << "]";
            if (funcIdx < module.functions.size())
                out << " " << module.functions[funcIdx].name;
            out << " base=" << base
                << " outMask=0x" << std::hex << outMask
                << std::dec << "\n";
            break;
        }

        //uint16 info
        case OpCode::OP_DebugInfo: {
            uint16_t info = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << info << "\n";
            break;
        }

        //uint16 switch_local_offset
        case OpCode::OP_Switch: {
            uint16_t off = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << off << "\n";
            break;
        }

        //uint16 jump_to_next
        case OpCode::OP_Case: {
            uint16_t next = reader.ReadUint16();
            char nextBuf[16];
            snprintf(nextBuf, sizeof(nextBuf), "%04x", static_cast<unsigned>(next));
            out << offsetBuf << ": " << name
                << " ->" << nextBuf << "\n";
            break;
        }

        //String operations: uint16 lhs/or dst, uint16 rhs/or src
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_Less_str:
        case OpCode::OP_LessEqual_str:
        case OpCode::OP_Greater_str:
        case OpCode::OP_GreaterEqual_str:
        case OpCode::OP_StrLen: {
            uint16_t a = reader.ReadUint16();
            uint16_t b = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << a << " " << b << "\n";
            break;
        }

        //Struct operations
        case OpCode::OP_AllocStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            uint16_t fieldCount = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << " struct=" << structIdx
                << " fields=" << fieldCount << "\n";
            break;
        }

        case OpCode::OP_LoadField: {
            uint16_t dst = reader.ReadUint16();
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << " obj=" << obj
                << " off=" << fieldOff << "\n";
            break;
        }

        case OpCode::OP_StoreField: {
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " obj=" << obj << " off=" << fieldOff
                << " " << src << "\n";
            break;
        }

        case OpCode::OP_CopyStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << " " << src
                << " struct=" << structIdx << "\n";
            break;
        }

        //Class/object operations
        case OpCode::OP_New: {
            uint16_t dst = reader.ReadUint16();
            uint16_t classIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << dst << " class=" << classIdx;
            if (classIdx < module.classes.size())
                out << " " << module.classes[classIdx].name;
            out << "\n";
            break;
        }

        case OpCode::OP_CallMethod: {
            uint16_t methodIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " method=" << methodIdx;
            if (methodIdx < module.stringConstants.size())
                out << " \"" << module.stringConstants[methodIdx] << "\"";
            out << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_CallMethodDirect: {
            uint16_t funcIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " [" << funcIdx << "]";
            if (funcIdx < module.functions.size())
                out << " " << module.functions[funcIdx].name;
            out << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_CallIntrinsic: {
            uint16_t id = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " id=" << id << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_Enum_to_str: {
            uint16_t enumDefIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " enumDefIdx=" << enumDefIdx;
            if (enumDefIdx < module.enumNames.size())
                out << " (values=" << module.enumNames[enumDefIdx].size() << ")";
            out << "\n";
            break;
        }

        case OpCode::OP_Array_to_str: {
            out << offsetBuf << ": " << name << "\n";
            break;
        }

        case OpCode::OP_NullCheck: {
            uint16_t obj = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " " << obj << "\n";
            break;
        }

        case OpCode::OP_AllocArray: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arrayTypeIdx = reader.ReadUint16();
            uint16_t sizeSlot = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " dst=" << dst << " type=" << arrayTypeIdx
                << " sizeSlot=" << sizeSlot << "\n";
            break;
        }

        case OpCode::OP_LoadElement: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " dst=" << dst << " arr=" << arr
                << " idx=" << index << "\n";
            break;
        }

        case OpCode::OP_StoreElement: {
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " arr=" << arr << " idx=" << index
                << " src=" << src << "\n";
            break;
        }

        case OpCode::OP_ArrayLength: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " dst=" << dst << " arr=" << arr << "\n";
            break;
        }

        case OpCode::OP_Box: {
            uint8_t typeTag = reader.ReadByte();
            out << offsetBuf << ": " << name
                << " typeTag=" << static_cast<int>(typeTag) << "\n";
            break;
        }

        case OpCode::OP_Unbox: {
            uint8_t typeTag = reader.ReadByte();
            out << offsetBuf << ": " << name
                << " typeTag=" << static_cast<int>(typeTag) << "\n";
            break;
        }

        case OpCode::OP_CheckCast: {
            uint16_t classIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " classIdx=" << classIdx << "\n";
            break;
        }

        //Phase 9d: exception-handling opcodes.
        case OpCode::OP_Throw: {
            uint16_t src = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " src=" << src << "\n";
            break;
        }
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            out << offsetBuf << ": " << name << "\n";
            break;

        //Phase 13: first-class function value opcodes.
        case OpCode::OP_MakeFunc: {
            uint16_t funcIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " funcIdx=" << funcIdx;
            if (funcIdx < module.functions.size())
                out << " (" << module.functions[funcIdx].name << ")";
            out << "\n";
            break;
        }
        case OpCode::OP_CallDelegate: {
            uint16_t callee = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " callee=" << callee << " base=" << base << "\n";
            break;
        }
        case OpCode::OP_Eq_func:
        case OpCode::OP_Ne_func: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " lhs=" << lhs << " rhs=" << rhs << "\n";
            break;
        }
        case OpCode::OP_Func_to_str:
            out << offsetBuf << ": " << name << "\n";
            break;
        case OpCode::OP_MakeBoundFunc: {
            uint16_t funcIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " funcIdx=" << funcIdx;
            if (funcIdx < module.functions.size())
                out << " (" << module.functions[funcIdx].name << ")";
            out << "\n";
            break;
        }
        case OpCode::OP_MakeVFunc: {
            uint16_t nameIdx = reader.ReadUint16();
            out << offsetBuf << ": " << name
                << " nameIdx=" << nameIdx;
            if (nameIdx < module.stringConstants.size())
                out << " (" << module.stringConstants[nameIdx] << ")";
            out << "\n";
            break;
        }
        case OpCode::OP_CallDelegateOut: {
            uint16_t callee = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            uint32_t outMask = reader.ReadUint32();
            out << offsetBuf << ": " << name
                << " callee=" << callee << " base=" << base
                << " outMask=0x" << std::hex << outMask << std::dec
                << "\n";
            break;
        }

        default:
            out << offsetBuf << ": unknown_op("
                << static_cast<int>(op) << ")\n";
            break;
        }

        std::string text = out.str();
        if (!text.empty() && text.back() == '\n')
            text.pop_back();
        lines.push_back({static_cast<uint32_t>(offset), std::move(text)});
    }
    return lines;
}

std::string DisassembleTryBlocks(const CompiledFunction& func) {
    std::ostringstream out;
    //Phase 9d: dump tryBlocks exception-handling table.
    if (!func.tryBlocks.empty()) {
        out << "  try blocks:\n";
        for (const auto& tb : func.tryBlocks) {
            out << "    [" << tb.startPc << ".." << tb.endPc
                << ") handler=" << tb.handlerPc
                << " class=" << tb.exceptionClassIdx
                << " catchLocal=" << tb.catchLocalOff << "\n";
        }
    }
    return out.str();
}

//Read-side twin of VmBackend.cpp's compiler-side static walk (used by
//RemapBytecode); the two copies stay separate — merging would couple
//the front end to the read side. ONE deviation from the compiler copy:
//the compiler-side default arm asserts (fine for a trusted self-built
//module), but a debugger walking arbitrary bytecode must fail soft, so
//the unknown-opcode arm throws instead.
size_t InstructionStride(OpCode op) {
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
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
            return 1 + 1;  // uint8 tag
        case OpCode::OP_Jump:
        case OpCode::OP_Case:
            return 1 + 2;  // int16 / uint16
        case OpCode::OP_ConstInt32:
        case OpCode::OP_ConstFloat:
            return 1 + 4;
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
            //First pc wins: entries ascend, only push unseen lines.
            if (map.empty() || map.back().line != line)
                map.push_back({line, static_cast<uint16_t>(pc)});
        }
        size_t stride = InstructionStride(op);
        if (stride == 0) break;  //defensive: never loop forever
        pc += stride;
    }
    return map;
}

} // namespace nlang
