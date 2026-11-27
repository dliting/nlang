// --- shared disassembler (ndisasm CLI + ndb `x`): per-opcode switches split
// into operand-domain functions; Emit* helpers printf-format one shape.

#include <nlang/core/Disassembler.h>
#include <nlang/core/BytecodeReader.h>
#include "DisassemblerPrim.h"
#include "DisassemblerFormat.h"
#include "DisassemblerCalls.h"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

//--- operand-shape printers: one operand shape per helper; the line is
//"head + formatted operands + newline" (head = "0042: name"). The
//shared vocabulary (EmitPlain/DisasmU16xN/EmitU8Tag) lives in
//DisassemblerFormat; the call and func-value families live in
//DisassemblerCalls (0.13.1a split).

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

//--- module-table printers: append the looked-up name when idx is in range.
static void EmitConstString(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t idx = r.ReadUint16();
    out << head << " [" << idx << "]";
    if (idx < module.stringConstants.size())
        out << " \"" << module.stringConstants[idx] << "\"";
    out << "\n";
}

static void EmitNew(BytecodeReader &r, std::ostringstream &out,
    const std::string &head, const CompiledModule &module) {
    uint16_t dst = r.ReadUint16();
    uint16_t classIdx = r.ReadUint16();
    uint16_t descIdx = r.ReadUint16();
    out << head << " " << dst << " class=" << classIdx;
    if (classIdx < module.classes.size())
        out << " " << module.classes[classIdx].name;
    //v0.13.1: the instantiation descriptor row (0xFFFF = none; composition
    //rows compose at run time, so the spelling prints as a row tag).
    if (descIdx != 0xFFFF
        && descIdx < module.descriptorTable.size()) {
        const auto& row = module.descriptorTable[descIdx];
        if (row.composition.empty())
            out << " desc=" << row.spelling;
        else
            out << " desc=composition#" << descIdx;
    } else
        out << " desc=none";
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

//--- operand-domain dispatch: true = handled, false tries the next.

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
            DisasmU16xN(1, r, out, head, " %u");
            return true;
        case OpCode::OP_Case:
            EmitCaseNext(r, out, head);
            return true;
        case OpCode::OP_AssertFail:
        case OpCode::OP_DebugInfo:
            DisasmU16xN(1, r, out, head, " %u");
            return true;
        case OpCode::OP_Throw:
            DisasmU16xN(1, r, out, head, " src=%u");
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
            DisasmU16xN(1, r, out, head, " %u");
            return true;
        default:
            return false;
    }
}

//0.7.5 kind-immediate printers live in DisassemblerPrim.cpp (size-split).

static bool DisasmArithOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head) {
    switch (op) {
        //kind, dst, src (binary arithmetic)
        case OpCode::OP_Add:
        case OpCode::OP_Sub:
        case OpCode::OP_Mul:
        case OpCode::OP_Div:
        case OpCode::OP_Mod:
        case OpCode::OP_Shl:
        case OpCode::OP_Shr:
        case OpCode::OP_BitAnd:
        case OpCode::OP_BitOr:
        case OpCode::OP_BitXor:
            DisasmPrimBinOp(r, out, head);
            return true;
        //kind, cmpOp, lhs, rhs (comparison)
        case OpCode::OP_Cmp:
            DisasmPrimCmp(r, out, head);
            return true;
        //kind, dst (unary)
        case OpCode::OP_Neg:
        case OpCode::OP_BitNot:
            DisasmPrimNeg(r, out, head);
            return true;
        case OpCode::OP_LogicalNot:
            DisasmU16xN(1, r, out, head, " %u");
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
            DisasmU16xN(2, r, out, head, " %u %u");
            return true;
        //0.7.5 char bridge
        case OpCode::OP_StrByteAt:
            DisasmU16xN(3, r, out, head, " dst=%u str=%u idx=%u");
            return true;
        case OpCode::OP_StrForeachStep:
            DisasmU16xN(4, r, out, head, " str=%u off=%u cond=%u ch=%u");
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

//v0.13.1: the runtime type-argument opcodes — typeName arms (dst is a
//frame offset; the ordinal is the declared type-parameter position, the
//composition index a descriptor-table row) and the D6 kind-dispatched T
//operations (0xFFFF marks the null-literal side of an equality).
static bool DisasmTypeArgOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head) {
    switch (op) {
        case OpCode::OP_TypeNameArg: {
            uint16_t dst = r.ReadUint16();
            uint8_t ordinal = r.ReadByte();
            out << head << " dst=" << dst << " ordinal="
                << static_cast<unsigned>(ordinal) << "\n";
            return true;
        }
        case OpCode::OP_TypeNameCompose:
            DisasmU16xN(2, r, out, head, " dst=%u composition=%u");
            return true;
        case OpCode::OP_Eq_T:
        case OpCode::OP_Ne_T:
            DisasmU16xN(4, r, out, head,
                " dst=%u ordinal=%u lhs=%u rhs=%u");
            return true;
        case OpCode::OP_TypeArg_to_str:
        case OpCode::OP_As_TypeParam:
            EmitU8Tag(r, out, head);
            return true;
        default:
            return false;
    }
}

//v0.13.9 phase ③: the byref slot family — dst/src pairs, the
//cell-offset builders, and the kind-tagged dereference pair.
static bool DisasmRefOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head) {
    switch (op) {
        case OpCode::OP_RefOfLocal:
        case OpCode::OP_RefOfPromoted:
        case OpCode::OP_RefHandle:
            DisasmU16xN(2, r, out, head, " dst=%u src=%u");
            return true;
        case OpCode::OP_RefOfField:
            DisasmU16xN(3, r, out, head, " dst=%u base=%u cell=%u");
            return true;
        case OpCode::OP_RefOfField_T:
            //Task 13: the generic-body field bind — ordinal + three slots.
            DisasmTagU16xN(3, r, out, head,
                           " ordinal=%d dst=%u base=%u cell=%u");
            return true;
        case OpCode::OP_RefOfElement:
            DisasmU16xN(3, r, out, head, " dst=%u arr=%u flat=%u");
            return true;
        case OpCode::OP_RefOfElementN:
            DisasmU16xN(4, r, out, head,
                        " dst=%u arr=%u idxBase=%u rank=%u");
            return true;
        case OpCode::OP_LoadRef:
        case OpCode::OP_StoreRef:
            DisasmTagU16xN(2, r, out, head, " typeTag=%d dst=%u src=%u");
            return true;
        case OpCode::OP_LoadRef_T:
        case OpCode::OP_StoreRef_T:
            //Task 8: the T& read/store — ordinal byte + two slot operands.
            DisasmTagU16xN(2, r, out, head, " ordinal=%d dst=%u src=%u");
            return true;
        default:
            return false;
    }
}

static bool DisasmObjectOps(OpCode op, BytecodeReader &r,
    std::ostringstream &out, const std::string &head,
    const CompiledModule &module) {
    switch (op) {
        case OpCode::OP_AllocStruct:
            DisasmU16xN(3, r, out, head, " %u struct=%u fields=%u");
            return true;
        case OpCode::OP_LoadField:
            DisasmU16xN(3, r, out, head, " %u obj=%u off=%u");
            return true;
        case OpCode::OP_StoreField:
            DisasmU16xN(3, r, out, head, " obj=%u off=%u %u");
            return true;
        case OpCode::OP_CopyStruct:
            DisasmU16xN(3, r, out, head, " %u %u struct=%u");
            return true;
        case OpCode::OP_CopyStructInPlace:
            DisasmU16xN(3, r, out, head, " %u %u struct=%u");
            return true;
        case OpCode::OP_New:
            EmitNew(r, out, head, module);
            return true;
        case OpCode::OP_NullCheck:
            DisasmU16xN(1, r, out, head, " %u");
            return true;
        case OpCode::OP_CheckCast:
            //v0.13.1: classIdx + descIdx (0xFFFF = no descriptor check).
            DisasmU16xN(2, r, out, head, " classIdx=%u desc=%u");
            return true;
        case OpCode::OP_Box:
        case OpCode::OP_Unbox:
        case OpCode::OP_Box_TypeArg:
        case OpCode::OP_Unbox_TypeArg:
            EmitU8Tag(r, out, head);
            return true;
        default:
            return DisasmTypeArgOps(op, r, out, head);
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
            !DisasmRefOps(op, reader, out, head) &&
            !DisasmArrayRecordOps(op, reader, out, head) &&
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
