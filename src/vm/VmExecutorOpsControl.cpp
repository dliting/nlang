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
    //iteration. Check GC here (tempSlot is consumed at this point); the
    //scheduler quota rides the same site (K1 decision 5) — GC keeps its
    //exact former position and ordering.
    if (target <= reader.CurrentOffset()) {
        CheckGCSafepoint();
        //The quota check below may park mid-instruction — park the
        //resume point at the jump destination FIRST, or a resumed
        //context would re-execute the whole iteration and hit the same
        //exhausted quota forever (zero-progress ping-pong; the fresh
        //worker root restarts at pc 0 outright). On the non-park path
        //the value is inert — normal flow seeks via the reader.
        m_callStack.back().pc = target;
        CheckSchedulerQuota();
    }
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
    //K1 decision 7: main context only (DebugHooksActive).
    if (DebugHooksActive()) {
        DebugStopInfo stop;
        stop.pc = opPc;
        stop.line = line;
        stop.funcIdx = CurrentFuncIdx();
        stop.depth = m_callStack.size();
        m_pDebugHooks->OnStatement(stop, *this);
    }
}

//K1 decision 5 — back-edge quota. Called only from the back-edge
//safepoint beside CheckGCSafepoint (OpJump). Nested evaluation windows
//are exempt: a park there is impossible (RequestPark would reject), so
//counting their back-edges would only misattribute budget — and in a
//multi-context run, exhaust the quota mid-instruction where preemption
//cannot be honored. K2 slice 2: the budget is per carrier (scheduler
//slot). K2 slice 3 (spec D6): yield on queued work ONLY. Every wake
//path (timer expiry, join/mutex/channel, future blocking-native
//completion) enqueues the woken context before notifying, so a
//non-empty run queue is the complete work signal. Yielding to a bare
//GIL waiter is self-sustaining churn: with carriers > 1 the
//non-running carrier is structurally always mid-lock-acquire (it must
// take the GIL to discover the queue is empty and cv-wait), so a
//waiter-count rule preempts every quota fire — park, frame steal,
//requeue, two lock handoffs, thread switches — the GIL convoy effect
// (cf. the pre-3.2 CPython "GIL battle", fixed by keying handoff on
//work signals, not lock contention). GilWaiters stays pure telemetry.
//Quota fires also service due timers on the spot — the timer thread
//cannot acquire the GIL while this carrier runs (a cv-internal
//reacquisition is invisible to any accounting), so whoever holds the
//lock does the expiry; the enqueued wakeups then drive the ordinary
//preempt path below.
void VmExecutor::CheckSchedulerQuota() {
    if (m_spScheduler->CarrierNestDepthRef() > 0)
        return;
    int& budget = m_spScheduler->CarrierTripBudgetRef();
    if (--budget > 0)
        return;
    budget = m_schedTripLimit;
    ++m_quotaFires;
    m_spScheduler->ServiceDueTimers();
    if (!m_spScheduler->HasOtherRunnable()) {
        //Nothing queued anywhere: reload and keep running. A
        //single-context program stays bit-identical — no signal, no
        //state change.
        ++m_preemptSkips;
        return;
    }
    RequestPark(ParkSignal::Reason::Preempted);
}

//O2-audit chokepoint — see VmExecutor.h. The quota path never reaches
//here nested (CheckSchedulerQuota exempts it); the guard serves the
//thread intrinsics from Slice 6 on (yield/sleep/join/recv called inside
//a toString body or a host callback must fail loudly, not park).
//K1 决策 2 实现规约 (a)：内在函数必须「完成或挂起」二态——禁止部分变
//更调度器状态后再抛。会先变异状态的挂起（join/sleep 的 ParkFor* 登记）
//必须在变异之前过本门；RequestPark 内保留同一检查作为零变异直挂路径
//（yield）的唯一守卫与纵深防御。K2 slice 2: 嵌套深度读运行载体的槽。
void VmExecutor::EnsureSuspensionAllowed() const {
    if (m_spScheduler->CarrierNestDepthRef() > 0)
        throw std::runtime_error(
            "NLang VM: cannot suspend inside a nested evaluation "
            "(toString or host callback)");
}

void VmExecutor::RequestPark(ParkSignal::Reason reason) {
    EnsureSuspensionAllowed();
    throw ParkSignal(reason);
}

} // namespace nlang
