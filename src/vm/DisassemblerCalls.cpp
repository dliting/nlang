/*---
    DisassemblerCalls.cpp — the call and func-value operand families
    (0.13.1a split from Disassembler.cpp; the emitters print the
    descriptor operand the frame-cell channel carries).
---*/
#include "DisassemblerCalls.h"
#include "DisassemblerFormat.h"

namespace nlang {

static void EmitCallDirect(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t funcIdx = r.ReadUint16();
    uint16_t base = r.ReadUint16();
    uint16_t descIdx = r.ReadUint16();
    out << head << " [" << funcIdx << "]";
    if (funcIdx < module.functions.size())
        out << " " << module.functions[funcIdx].name;
    out << " base=" << base << " desc=0x" << std::hex << descIdx
        << std::dec;
    out << "\n";
}

//Bound-method direct call: "[i] name base=b" — no descriptor operand
//(the receiver fills the cell).
static void EmitCallMethodDirect(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t funcIdx = r.ReadUint16();
    uint16_t base = r.ReadUint16();
    out << head << " [" << funcIdx << "]";
    if (funcIdx < module.functions.size())
        out << " " << module.functions[funcIdx].name;
    out << " base=" << base;
    out << "\n";
}

//By-name method call: method=<string-pool idx> "name" base=b.
static void EmitCallByName(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t methodIdx = r.ReadUint16();
    uint16_t base = r.ReadUint16();
    out << head << " method=" << methodIdx;
    if (methodIdx < module.stringConstants.size())
        out << " \"" << module.stringConstants[methodIdx] << "\"";
    out << " base=" << base << "\n";
}

//funcIdx line with "(name)" suffix (MakeFunc/MakeBoundFunc share it).
static void EmitMakeFuncIdx(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t funcIdx = r.ReadUint16();
    out << head << " funcIdx=" << funcIdx;
    if (funcIdx < module.functions.size())
        out << " (" << module.functions[funcIdx].name << ")";
    out << "\n";
}

static void EmitMakeVFunc(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t nameIdx = r.ReadUint16();
    out << head << " nameIdx=" << nameIdx;
    if (nameIdx < module.stringConstants.size())
        out << " (" << module.stringConstants[nameIdx] << ")";
    out << "\n";
}

bool DisasmCallOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_CallStatic:
            EmitCallDirect(r, out, head, module);
            return true;
        case OpCode::OP_CallMethodDirect:
            EmitCallMethodDirect(r, out, head, module);
            return true;
        case OpCode::OP_CallMethod:
            EmitCallByName(r, out, head, module);
            return true;
        case OpCode::OP_CallIntrinsic:
            DisasmU16xN(2, r, out, head, " id=%u base=%u");
            return true;
        case OpCode::OP_CallDelegate:
            DisasmU16xN(2, r, out, head, " callee=%u base=%u");
            return true;
        default:
            return false;
    }
}

bool DisasmFuncValueOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_MakeFunc:
        case OpCode::OP_MakeBoundFunc:
            EmitMakeFuncIdx(r, out, head, module);
            return true;
        case OpCode::OP_MakeVFunc:
            EmitMakeVFunc(r, out, head, module);
            return true;
        case OpCode::OP_Eq_func:
        case OpCode::OP_Ne_func:
        case OpCode::OP_Eq_struct:
        case OpCode::OP_Ne_struct:
            DisasmU16xN(2, r, out, head, " lhs=%u rhs=%u");
            return true;
        case OpCode::OP_Func_to_str:
            EmitPlain(out, head);
            return true;
        default:
            return false;
    }
}

} // namespace nlang
