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
};

static const char* TypeKindName(uint16_t kind) {
    if (kind <= 2)
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
