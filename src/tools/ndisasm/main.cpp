#include "ModuleLoader.h"
#include "BytecodeReader.h"
#include "BytecodeOps.h"
#include <cstdio>
#include <iostream>
#include <string>

using namespace nlang;

static const char* s_typeKindNames[] = {
    "i32",    // NK_Int32 = 0 (also default for void-like functions)
    "f32",    // NK_Float = 1
    "str",    // NK_String = 2
    "struct", // RTK_Struct = 3
    "class"   // RTK_Class = 4
};

static const char* TypeKindName(uint16_t kind) {
    if (kind <= 4)
        return s_typeKindNames[kind];
    return "unknown";
}

static void DisassembleFunction(const CompiledFunction& func,
                                 const CompiledModule& module) {
    std::cout << "function " << func.name
              << " (frameSize=" << func.localsSize
              << ", params=" << func.paramCount
              << ", returnType=" << TypeKindName(func.returnTypeKind)
              << ")\n";

    if (func.bytecode.empty()) {
        std::cout << "  (no bytecode)\n\n";
        return;
    }

    std::cout << "  bytecode:\n";

    BytecodeReader reader(func.bytecode.data(), func.bytecode.size());

    while (!reader.Eof()) {
        size_t offset = reader.CurrentOffset();

        //Pad offset to 4 hex digits
        char offsetBuf[16];
        snprintf(offsetBuf, sizeof(offsetBuf), "%04zx", offset);

        OpCode op = reader.ReadOp();
        const char* name = OpCodeName(op);

        switch (op) {
        //No operands
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ConstZero:
        case OpCode::OP_CastIntToFloat:
        case OpCode::OP_CastFloatToInt:
        case OpCode::OP_ParaEnd:
            std::cout << "    " << offsetBuf << ": " << name << "\n";
            break;

        //int16 target (jump)
        case OpCode::OP_Jump: {
            int16_t target = reader.ReadInt16();
            char targetBuf[16];
            snprintf(targetBuf, sizeof(targetBuf), "%04x",
                     static_cast<unsigned>(static_cast<int16_t>(target)));
            std::cout << "    " << offsetBuf << ": " << name
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
            std::cout << "    " << offsetBuf << ": " << name
                      << " ->" << targetBuf << " " << localOff << "\n";
            break;
        }

        //int32 value
        case OpCode::OP_ConstInt32: {
            int32_t v = reader.ReadInt32();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << v << "\n";
            break;
        }

        //float value
        case OpCode::OP_ConstFloat: {
            float v = reader.ReadFloat();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << v << "\n";
            break;
        }

        //uint16 pool_index (string constant)
        case OpCode::OP_ConstString: {
            uint16_t idx = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " [" << idx << "]";
            if (idx < module.stringConstants.size())
                std::cout << " \"" << module.stringConstants[idx] << "\"";
            std::cout << "\n";
            break;
        }

        //uint16 offset (variable access)
        case OpCode::OP_VarLocal: {
            uint16_t off = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << off << "\n";
            break;
        }

        //uint16 dst_offset (assign)
        case OpCode::OP_Assign: {
            uint16_t dst = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
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
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << dst << " " << src << "\n";
            break;
        }

        //uint16 dst (unary)
        case OpCode::OP_Neg_i32:
        case OpCode::OP_Neg_f32:
        case OpCode::OP_LogicalNot: {
            uint16_t dst = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
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
        case OpCode::OP_NotEqual_f32:
        case OpCode::OP_LogicalAnd:
        case OpCode::OP_LogicalOr: {
            uint16_t lhs = reader.ReadUint16();
            uint16_t rhs = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << lhs << " " << rhs << "\n";
            break;
        }

        //uint16 func_index, uint16 call_param_base
        case OpCode::OP_CallFunc: {
            uint16_t funcIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " [" << funcIdx << "]";
            if (funcIdx < module.functions.size())
                std::cout << " " << module.functions[funcIdx].name;
            std::cout << " base=" << base << "\n";
            break;
        }

        //uint16 info
        case OpCode::OP_DebugInfo: {
            uint16_t info = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << info << "\n";
            break;
        }

        //uint16 switch_local_offset
        case OpCode::OP_Switch: {
            uint16_t off = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << off << "\n";
            break;
        }

        //uint16 jump_to_next
        case OpCode::OP_Case: {
            uint16_t next = reader.ReadUint16();
            char nextBuf[16];
            snprintf(nextBuf, sizeof(nextBuf), "%04x", static_cast<unsigned>(next));
            std::cout << "    " << offsetBuf << ": " << name
                      << " ->" << nextBuf << "\n";
            break;
        }

        //String operations: uint16 lhs/or dst, uint16 rhs/or src
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_StrLen: {
            uint16_t a = reader.ReadUint16();
            uint16_t b = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << a << " " << b << "\n";
            break;
        }

        //Struct operations
        case OpCode::OP_AllocStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            uint16_t fieldCount = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << dst << " struct=" << structIdx
                      << " fields=" << fieldCount << "\n";
            break;
        }

        case OpCode::OP_LoadField: {
            uint16_t dst = reader.ReadUint16();
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << dst << " obj=" << obj
                      << " off=" << fieldOff << "\n";
            break;
        }

        case OpCode::OP_StoreField: {
            uint16_t obj = reader.ReadUint16();
            uint16_t fieldOff = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " obj=" << obj << " off=" << fieldOff
                      << " " << src << "\n";
            break;
        }

        case OpCode::OP_CopyStruct: {
            uint16_t dst = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            uint16_t structIdx = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << dst << " " << src
                      << " struct=" << structIdx << "\n";
            break;
        }

        //Class/object operations
        case OpCode::OP_New: {
            uint16_t dst = reader.ReadUint16();
            uint16_t classIdx = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << dst << " class=" << classIdx;
            if (classIdx < module.classes.size())
                std::cout << " " << module.classes[classIdx].name;
            std::cout << "\n";
            break;
        }

        case OpCode::OP_CallMethod: {
            uint16_t methodIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " method=" << methodIdx;
            if (methodIdx < module.stringConstants.size())
                std::cout << " \"" << module.stringConstants[methodIdx] << "\"";
            std::cout << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_CallMethodDirect: {
            uint16_t funcIdx = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " [" << funcIdx << "]";
            if (funcIdx < module.functions.size())
                std::cout << " " << module.functions[funcIdx].name;
            std::cout << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_CallIntrinsic: {
            uint16_t id = reader.ReadUint16();
            uint16_t base = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " id=" << id << " base=" << base << "\n";
            break;
        }

        case OpCode::OP_NullCheck: {
            uint16_t obj = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " " << obj << "\n";
            break;
        }

        case OpCode::OP_AllocArray: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arrayTypeIdx = reader.ReadUint16();
            uint16_t sizeSlot = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " dst=" << dst << " type=" << arrayTypeIdx
                      << " sizeSlot=" << sizeSlot << "\n";
            break;
        }

        case OpCode::OP_LoadElement: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " dst=" << dst << " arr=" << arr
                      << " idx=" << index << "\n";
            break;
        }

        case OpCode::OP_StoreElement: {
            uint16_t arr = reader.ReadUint16();
            uint16_t index = reader.ReadUint16();
            uint16_t src = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " arr=" << arr << " idx=" << index
                      << " src=" << src << "\n";
            break;
        }

        case OpCode::OP_ArrayLength: {
            uint16_t dst = reader.ReadUint16();
            uint16_t arr = reader.ReadUint16();
            std::cout << "    " << offsetBuf << ": " << name
                      << " dst=" << dst << " arr=" << arr << "\n";
            break;
        }

        default:
            std::cout << "    " << offsetBuf << ": unknown_op("
                      << static_cast<int>(op) << ")\n";
            break;
        }
    }

    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ndisasm <module.nmod>\n"
                  << "       ndisasm -func <name> <module.nmod>\n";
        return 1;
    }

    std::string funcFilter;
    std::string modulePath;

    if (std::string(argv[1]) == "-func" && argc >= 4) {
        funcFilter = argv[2];
        modulePath = argv[3];
    } else {
        modulePath = argv[1];
    }

    CompiledModule module;
    try {
        module = ModuleLoader::Load(modulePath);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "module: " << module.name << "\n";

    //Struct descriptors
    if (module.structs.empty()) {
        std::cout << "structs: (none)\n";
    } else {
        std::cout << "structs:\n";
        for (size_t i = 0; i < module.structs.size(); ++i) {
            auto& st = module.structs[i];
            std::cout << "  [" << i << "] " << st.name
                      << " (fields=" << st.fieldCount << ")\n";
            for (uint16_t j = 0; j < st.fieldCount; ++j) {
                std::cout << "    " << st.fieldNames[j]
                          << ": " << TypeKindName(st.fieldTypeKinds[j]);
                if (st.fieldTypeKinds[j] == RTK_Struct
                    && st.fieldStructIndices[j] != 0xFFFF)
                    std::cout << " [" << st.fieldStructIndices[j] << "]";
                std::cout << "\n";
            }
        }
    }
    std::cout << "\n";

    //Class descriptors
    if (module.classes.empty()) {
        std::cout << "classes: (none)\n";
    } else {
        std::cout << "classes:\n";
        for (size_t i = 0; i < module.classes.size(); ++i) {
            auto& cc = module.classes[i];
            std::cout << "  [" << i << "] " << cc.name
                      << " (fields=" << cc.fieldCount
                      << ", super=" << cc.superClassIdx << ")\n";
            for (uint16_t j = 0; j < cc.fieldCount; ++j) {
                std::cout << "    " << cc.fieldNames[j]
                          << ": " << TypeKindName(cc.fieldTypeKinds[j]);
                if (cc.fieldTypeKinds[j] == RTK_Struct
                    && cc.fieldStructIndices[j] != 0xFFFF)
                    std::cout << " [" << cc.fieldStructIndices[j] << "]";
                if (cc.fieldTypeKinds[j] == RTK_Class
                    && cc.fieldClassIndices[j] != 0xFFFF)
                    std::cout << " [" << cc.fieldClassIndices[j] << "]";
                std::cout << "\n";
            }
        }
    }
    std::cout << "\n";

    //String constants
    if (module.stringConstants.empty()) {
        std::cout << "string constants: (none)\n";
    } else {
        std::cout << "string constants:\n";
        for (size_t i = 0; i < module.stringConstants.size(); ++i) {
            std::cout << "  [" << i << "] \"" << module.stringConstants[i]
                      << "\"\n";
        }
    }
    std::cout << "\n";

    //Functions
    for (auto& func : module.functions) {
        if (!funcFilter.empty() && func.name != funcFilter)
            continue;
        DisassembleFunction(func, module);
    }

    return 0;
}
