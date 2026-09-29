/*---
    VmExecutorOpsControl.cpp — 常量装载、赋值、跳转与调试信息操作码
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpAssertFail(BytecodeReader& reader) {
    //msgIdx is a compile-time constant index — resolve through the
    //constant cache like OP_ConstString does.
    uint16_t msgIdx = reader.ReadUint16();
    std::string msg = "assertion failed";
    if (msgIdx < m_constStrCache.size()) {
        const std::string& s = StrVal(m_constStrCache[msgIdx]);
        if (!s.empty())
            msg += ": " + s;
    }
    RaiseNlangException(m_assertExcClassIdx, msg);
}

void VmExecutor::OpConstInt32(BytecodeReader& reader, uint8_t* pResult) {
    int32_t v = reader.ReadInt32();
    std::memcpy(pResult, &v, sizeof(v));
}

void VmExecutor::OpConstFloat(BytecodeReader& reader, uint8_t* pResult) {
    float v = reader.ReadFloat();
    std::memcpy(pResult, &v, sizeof(v));
}

void VmExecutor::OpConstString(BytecodeReader& reader, uint8_t* pResult) {
    uint16_t poolIdx = reader.ReadUint16();
    if (poolIdx >= m_constStrCache.size())
        throw std::runtime_error(
            "NLang VM: string constant index out of range");
    int32_t handle = m_constStrCache[poolIdx];
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpVarLocal(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    //Uniform frame slots: copy the whole 8-byte cell. 4-byte kinds
    //keep their value in the low half; the upper-half bytes may be
    //garbage, which is harmless — kind-correct consumers never read
    //them (see kFrameSlotBytes in CompiledModule.h).
    uint16_t offset = reader.ReadUint16();
    std::memcpy(pResult, locals + offset, kFrameSlotBytes);
}

void VmExecutor::OpAssign(BytecodeReader& reader, uint8_t* locals, uint8_t* pResult) {
    uint16_t dstOffset = reader.ReadUint16();
    std::memcpy(locals + dstOffset, pResult, kFrameSlotBytes);
}

void VmExecutor::OpJump(BytecodeReader& reader) {
    uint16_t target = reader.ReadUint16();
    //Loop back-edge safepoint: if jumping backward, this is a loop
    //iteration. Check GC here (tempSlot is consumed at this point).
    if (target <= reader.CurrentOffset())
        CheckGCSafepoint();
    reader.Seek(target);
}

void VmExecutor::OpJumpIfNot(BytecodeReader& reader, uint8_t* locals) {
    uint16_t target = reader.ReadUint16();
    uint16_t localOff = reader.ReadUint16();
    int32_t cond;
    std::memcpy(&cond, locals + localOff, sizeof(cond));
    if (!cond)
        reader.Seek(target);
}

void VmExecutor::OpSwitch(BytecodeReader& reader) {
    //Read the switch value local offset — no action needed,
    //the switch value is already in the local slot.
    reader.ReadUint16();
}

void VmExecutor::OpCase(BytecodeReader& reader) {
    //Read the jump-to-next-handler offset.
    //The jump target is patched at compile time (clause-exit fixup).
    //At runtime, we just read and skip the placeholder — the actual
    //branching is done by OP_JumpIfNot after the condition code.
    reader.ReadUint16();
}

void VmExecutor::OpDebugInfo(BytecodeReader& reader, uint16_t opPc) {
    uint16_t line = reader.ReadUint16();
    if (!m_callStack.empty()) {
        m_callStack.back().currentLine = line;
        m_callStack.back().currentPc = opPc;
    }
    //Debugger checkpoint: the callback runs with the program
    //frozen at this statement; returning resumes in place.
    //Front ends must not let exceptions escape into the VM
    //(they would cross the NLang try/catch boundary).
    if (m_pDebugHooks) {
        DebugStopInfo stop;
        stop.pc = opPc;
        stop.line = line;
        stop.funcIdx = CurrentFuncIdx();
        stop.depth = m_callStack.size();
        m_pDebugHooks->OnStatement(stop, *this);
    }
}

} // namespace nlang
