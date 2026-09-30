// --- shared disassembler (ndisasm CLI + ndb `x`) ---
// Instruction-line formatting. The per-opcode switch is split into
// operand-domain functions (flow/constants/arithmetic/strings/calls/
// objects/func-values), each under the 50-line function limit; Emit*
// helpers read one operand shape and format it with a printf-style fmt.
// Output is byte-identical to the original single-switch version
// (guarded byte-for-byte by temp/dis_compare.py over the corpus).

#include "Disassembler.h"
#include "BytecodeReader.h"
#include "DisassemblerPrim.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

//--- operand-shape printers ---
//Each Emit* helper reads the operands of one shape from the reader and
//writes "head + formatted operands + newline" (head = "0042: name").

static void EmitPlain(std::ostringstream &out, const std::string &head) {
    out << head << "\n";
}

static void EmitU16(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const char *fmt) {
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, r.ReadUint16());
    out << head << buf << "\n";
}

//Print `n` uint16 operands (n = 2..4; the format consumes at most four).
//Reads into a named array BEFORE the format call: argument evaluation
//order is unspecified, so in-argument reads would emit the operands in
//reverse on right-to-left evaluators (MSVC). Unused array slots stay
//zeroed — snprintf ignores extra arguments.
static void EmitU16xN(int n, BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const char *fmt) {
    uint16_t v[4] = {0, 0, 0, 0};
    for (int i = 0; i < n; ++i)
        v[i] = r.ReadUint16();
    char buf[96];
    snprintf(buf, sizeof(buf), fmt, v[0], v[1], v[2], v[3]);
    out << head << buf << "\n";
}

static void EmitI32(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    char buf[48];
    snprintf(buf, sizeof(buf), " %d", r.ReadInt32());
    out << head << buf << "\n";
}

static void EmitF32(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    char buf[48];
    snprintf(buf, sizeof(buf), " %g", r.ReadFloat());
    out << head << buf << "\n";
}

static void EmitU8Tag(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    char buf[48];
    snprintf(buf, sizeof(buf), " typeTag=%d", r.ReadByte());
    out << head << buf << "\n";
}

//Sign-extended int16 jump target printed as 4 hex digits ("->0042").
static void EmitJumpTarget(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    int16_t target = r.ReadInt16();
    char buf[48];
    snprintf(buf, sizeof(buf), " ->%04x",
             static_cast<unsigned>(static_cast<int16_t>(target)));
    out << head << buf << "\n";
}

//int16 sign-extended hex target + uint16 local (conditional jump).
static void EmitJumpCond(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    int16_t target = r.ReadInt16();
    uint16_t localOff = r.ReadUint16();
    char buf[48];
    snprintf(buf, sizeof(buf), " ->%04x %u",
             static_cast<unsigned>(static_cast<int16_t>(target)), localOff);
    out << head << buf << "\n";
}

//uint16 jump-to-next printed as 4 hex digits (switch case chains).
static void EmitCaseNext(BytecodeReader &r, std::ostringstream &out,
    const std::string &head) {
    uint16_t next = r.ReadUint16();
    char buf[48];
    snprintf(buf, sizeof(buf), " ->%04x", static_cast<unsigned>(next));
    out << head << buf << "\n";
}

//--- module-table printers ---
//Lines that append a looked-up name from the module tables when the
//index is in range (out-of-range prints bare, matching the original).

static void EmitConstString(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t idx = r.ReadUint16();
    out << head << " [" << idx << "]";
    if (idx < module.stringConstants.size())
        out << " \"" << module.stringConstants[idx] << "\"";
    out << "\n";
}

//Direct function-index call: "[i] name base=b" (CallFunc/Direct).
static void EmitCallDirect(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t funcIdx = r.ReadUint16();
    uint16_t base = r.ReadUint16();
    out << head << " [" << funcIdx << "]";
    if (funcIdx < module.functions.size())
        out << " " << module.functions[funcIdx].name;
    out << " base=" << base << "\n";
}

//...with the Phase 9e uint32 out-mask tail (CallFuncOut/DirectOut).
static void EmitCallDirectOut(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t funcIdx = r.ReadUint16();
    uint16_t base = r.ReadUint16();
    uint32_t outMask = r.ReadUint32();
    out << head << " [" << funcIdx << "]";
    if (funcIdx < module.functions.size())
        out << " " << module.functions[funcIdx].name;
    out << " base=" << base
        << " outMask=0x" << std::hex << outMask
        << std::dec << "\n";
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

static void EmitNew(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t dst = r.ReadUint16();
    uint16_t classIdx = r.ReadUint16();
    out << head << " " << dst << " class=" << classIdx;
    if (classIdx < module.classes.size())
        out << " " << module.classes[classIdx].name;
    out << "\n";
}

static void EmitEnumToStr(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t enumDefIdx = r.ReadUint16();
    out << head << " enumDefIdx=" << enumDefIdx;
    if (enumDefIdx < module.enumNames.size())
        out << " (values=" << module.enumNames[enumDefIdx].size() << ")";
    out << "\n";
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

//--- operand-domain dispatch ---
//Each Disasm*Ops function owns one opcode domain: returns true (and
//writes the line) when it handled op, false to let the next domain try.

static bool DisasmFlowOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head) {
    switch (op) {
        case OpCode::OP_Return:
        case OpCode::OP_Stop:
        case OpCode::OP_ParaEnd:
        case OpCode::OP_Rethrow:
        case OpCode::OP_PopHandler:
            EmitPlain(out, head);
            return true;
        case OpCode::OP_Jump:
            EmitJumpTarget(r, out, head);
            return true;
        case OpCode::OP_JumpIfNot:
            EmitJumpCond(r, out, head);
            return true;
        case OpCode::OP_Switch:
            EmitU16(r, out, head, " %u");
            return true;
        case OpCode::OP_Case:
            EmitCaseNext(r, out, head);
            return true;
        case OpCode::OP_AssertFail:
        case OpCode::OP_DebugInfo:
            EmitU16(r, out, head, " %u");
            return true;
        case OpCode::OP_Throw:
            EmitU16(r, out, head, " src=%u");
            return true;
        default:
            return false;
    }
}

static bool DisasmConstOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_ConstZero:
            EmitPlain(out, head);
            return true;
        case OpCode::OP_ConstInt32:
            EmitI32(r, out, head);
            return true;
        case OpCode::OP_ConstFloat:
            EmitF32(r, out, head);
            return true;
        case OpCode::OP_ConstInt64: {
            char buf[48];
            snprintf(buf, sizeof(buf), " %lld",
                     static_cast<long long>(r.ReadInt64()));
            out << head << buf << "\n";
            return true;
        }
        case OpCode::OP_ConstDouble: {
            char buf[48];
            snprintf(buf, sizeof(buf), " %.17g", r.ReadDouble());
            out << head << buf << "\n";
            return true;
        }
        case OpCode::OP_ConstString:
            EmitConstString(r, out, head, module);
            return true;
        case OpCode::OP_VarLocal:
        case OpCode::OP_Assign:
            EmitU16(r, out, head, " %u");
            return true;
        default:
            return false;
    }
}

//--- 0.7.5 kind-immediate family printers live in DisassemblerPrim.cpp
//(file-size guard split); the call sites below route through them.

static bool DisasmArithOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head) {
    switch (op) {
        //kind, dst, src (binary arithmetic)
        case OpCode::OP_Add:
        case OpCode::OP_Sub:
        case OpCode::OP_Mul:
        case OpCode::OP_Div:
        case OpCode::OP_Mod:
            DisasmPrimBinOp(r, out, head);
            return true;
        //kind, cmpOp, lhs, rhs (comparison)
        case OpCode::OP_Cmp:
            DisasmPrimCmp(r, out, head);
            return true;
        //kind, dst (unary)
        case OpCode::OP_Neg:
            DisasmPrimNeg(r, out, head);
            return true;
        case OpCode::OP_LogicalNot:
            EmitU16(r, out, head, " %u");
            return true;
        default:
            return false;
    }
}

static bool DisasmStringOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        //lhs/or dst, rhs/or src
        case OpCode::OP_Concat_str:
        case OpCode::OP_Eq_str:
        case OpCode::OP_Ne_str:
        case OpCode::OP_Less_str:
        case OpCode::OP_LessEqual_str:
        case OpCode::OP_Greater_str:
        case OpCode::OP_GreaterEqual_str:
        case OpCode::OP_StrLen:
            EmitU16xN(2, r, out, head, " %u %u");
            return true;
        //0.7.5 char bridge
        case OpCode::OP_StrByteAt:
            EmitU16xN(3, r, out, head, " dst=%u str=%u idx=%u");
            return true;
        case OpCode::OP_StrForeachStep:
            EmitU16xN(4, r, out, head, " str=%u off=%u cond=%u ch=%u");
            return true;
        case OpCode::OP_Prim_to_str:
            DisasmPrimToStr(r, out, head);
            return true;
        case OpCode::OP_PrimCast:
            DisasmPrimCast(r, out, head);
            return true;
        case OpCode::OP_Array_to_str:
            EmitPlain(out, head);
            return true;
        case OpCode::OP_Enum_to_str:
            EmitEnumToStr(r, out, head, module);
            return true;
        default:
            return false;
    }
}

static bool DisasmCallOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_CallFunc:
        case OpCode::OP_CallMethodDirect:
            EmitCallDirect(r, out, head, module);
            return true;
        case OpCode::OP_CallFuncOut:
        case OpCode::OP_CallMethodDirectOut:
            EmitCallDirectOut(r, out, head, module);
            return true;
        case OpCode::OP_CallMethod:
            EmitCallByName(r, out, head, module);
            return true;
        case OpCode::OP_CallIntrinsic:
            EmitU16xN(2, r, out, head, " id=%u base=%u");
            return true;
        case OpCode::OP_CallDelegate:
            EmitU16xN(2, r, out, head, " callee=%u base=%u");
            return true;
        case OpCode::OP_CallDelegateOut: {
            uint16_t callee = r.ReadUint16();
            uint16_t base = r.ReadUint16();
            uint32_t outMask = r.ReadUint32();
            out << head << " callee=" << callee << " base=" << base
                << " outMask=0x" << std::hex << outMask << std::dec
                << "\n";
            return true;
        }
        default:
            return false;
    }
}

static bool DisasmObjectOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_AllocStruct:
            EmitU16xN(3, r, out, head, " %u struct=%u fields=%u");
            return true;
        case OpCode::OP_LoadField:
            EmitU16xN(3, r, out, head, " %u obj=%u off=%u");
            return true;
        case OpCode::OP_StoreField:
            EmitU16xN(3, r, out, head, " obj=%u off=%u %u");
            return true;
        case OpCode::OP_CopyStruct:
            EmitU16xN(3, r, out, head, " %u %u struct=%u");
            return true;
        case OpCode::OP_New:
            EmitNew(r, out, head, module);
            return true;
        case OpCode::OP_NullCheck:
            EmitU16(r, out, head, " %u");
            return true;
        case OpCode::OP_CheckCast:
            EmitU16(r, out, head, " classIdx=%u");
            return true;
        case OpCode::OP_AllocArray:
            EmitU16xN(3, r, out, head, " dst=%u type=%u sizeSlot=%u");
            return true;
        case OpCode::OP_LoadElement:
            EmitU16xN(3, r, out, head, " dst=%u arr=%u idx=%u");
            return true;
        case OpCode::OP_StoreElement:
            EmitU16xN(3, r, out, head, " arr=%u idx=%u src=%u");
            return true;
        case OpCode::OP_ArrayLength:
            EmitU16xN(2, r, out, head, " dst=%u arr=%u");
            return true;
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
            EmitU8Tag(r, out, head);
            return true;
        default:
            return false;
    }
}

static bool DisasmFuncValueOps(OpCode op, BytecodeReader &r,
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
            EmitU16xN(2, r, out, head, " lhs=%u rhs=%u");
            return true;
        case OpCode::OP_Func_to_str:
            EmitPlain(out, head);
            return true;
        default:
            return false;
    }
}

std::vector<DisasmLine> DisassembleCode(const CompiledFunction& func,
    const CompiledModule& module) {
    std::vector<DisasmLine> lines;
    BytecodeReader reader(func.bytecode.data(), func.bytecode.size());

    while (!reader.Eof()) {
        size_t offset = reader.CurrentOffset();
        //Pad offset to 4 hex digits
        char offsetBuf[16];
        snprintf(offsetBuf, sizeof(offsetBuf), "%04zx", offset);

        OpCode op = reader.ReadOp();
        const char* name = OpCodeName(op);
        std::string head = std::string(offsetBuf) + ": " + name;

        std::ostringstream out;
        if (!DisasmFlowOps(op, reader, out, head) &&
            !DisasmConstOps(op, reader, out, head, module) &&
            !DisasmArithOps(op, reader, out, head) &&
            !DisasmStringOps(op, reader, out, head, module) &&
            !DisasmCallOps(op, reader, out, head, module) &&
            !DisasmObjectOps(op, reader, out, head, module) &&
            !DisasmFuncValueOps(op, reader, out, head, module)) {
            out << offsetBuf << ": unknown_op("
                << static_cast<int>(op) << ")\n";
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

} // namespace nlang
