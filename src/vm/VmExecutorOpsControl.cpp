/*---
    VmExecutorOpsControl.cpp — control-flow opcodes: constant loads,
    assignment, jumps, exception dispatch (Phase 9d ops moved here
    from VmExecutorOpsObjects.cpp, 2026-10-25) and debug info.
    Extracted from VmExecutor.cpp (2026-09-26, zero behavior change).
    K8 lock classification (ported with the merge): the throw arms
    keep the store lock — FireOnThrow's debug checkpoint is
    serialized only by it; ThrownMessageOf itself is a lock-free
    heap read under the sharing model.
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
    //K8: lock-free — the constant cache is read-only after run setup,
    //and StrVal's reference is stable across concurrent mints (chunked
    //storage, flatten self-locks). The raise runs with no domain held.
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
        EvalStack().back().pc = target;
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
    if (!EvalStack().empty()) {
        EvalStack().back().currentLine = line;
        EvalStack().back().currentPc = opPc;
    }
    //Debugger checkpoint: the callback runs with the program
    //frozen at this statement; returning resumes in place.
    //Front ends must not let exceptions escape into the VM
    //(they would cross the NLang try/catch boundary).
    //K4: every context checkpoints (breakpoints and stepping cover
    //spawned contexts; the stop payload identifies which one).
    if (DebugHooksActive()) {
        DebugStopInfo stop;
        stop.contextId = m_spScheduler->CurrentContextId();
        stop.pc = opPc;
        stop.line = line;
        stop.funcIdx = CurrentFuncIdx();
        stop.depth = EvalStack().size();
        m_pDebugHooks->OnStatement(stop, *this);
        SyncDebugFocus();
    }
}

//K1 decision 5 — back-edge quota. Called only from the back-edge
//safepoint beside CheckGCSafepoint (OpJump). Nested evaluation windows
//are exempt: their frames are a bounded re-entrant slice (formatting,
//host bridge), and a window is one VM operation seen from its outer
//context — wait points suspend the whole window (P2-4), but quota
//preemption mid-window buys nothing, so counting their back-edges
//would only misattribute budget — and in a multi-context run, exhaust
//the quota mid-instruction where preemption is not honored. K2 slice 2: the budget is per carrier (scheduler
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

//O2-audit chokepoint — see VmExecutor.h. P2-4: nested windows carry a
//pump (RunNestedWindow) — suspension parks, the pump drives the
//scheduler, and the window resumes in place. The only surviving
//rejection is the carrier-less embed window (thread.spawn's guard
//documents the same shape): with no active carrier nothing can ever
//serve the park, so failing fast with a precise message beats an
//instant deadlock report.
//K1 决策 2 实现规约 (a)：内在函数必须「完成或挂起」二态——禁止部分变
//更调度器状态后再抛。会先变异状态的挂起（join/sleep 的 ParkFor* 登记）
//必须在变异之前过本门；RequestPark 内保留同一检查作为零变异直挂路径
//（yield）的唯一守卫与纵深防御。
void VmExecutor::EnsureSuspensionAllowed() const {
    if (m_spScheduler->ActiveCarrierCount() == 0)
        throw std::runtime_error(
            "NLang VM: cannot suspend inside a host call window with "
            "no active carrier");
}

void VmExecutor::RequestPark(ParkSignal::Reason reason) {
    EnsureSuspensionAllowed();
    throw ParkSignal(reason);
}

//v0.8.5: see the header. Reads the exception object's own message so
//the top-level report names the actual failure (the old fixed texts
//"user throw"/"rethrow" hid it).
//v0.12.4 D8: the report is now the full chain — FormatExceptionChain
//builds the "<Type>: <message>" top line plus the "Caused by:" hops,
//so an uncaught wrapped exception names every link.
std::string VmExecutor::ThrownMessageOf(int32_t heapIdx,
                                        const char* fallback) const {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Class)
        return fallback;
    return FormatExceptionChain(heapIdx);
}

//v0.12.4 D8: one hop's text — "<Type>: <message>", or the bare type
//name when the message is empty (ThrownMessageOf's empty-message rule,
//kept: a message-less exception still names itself). The type is the
//leaf spelling (the user-facing rendering rule the report always had —
//HostClassName's raw key is package-qualified).
std::string VmExecutor::FormatExceptionChain(int32_t excHeapIdx) const {
    //Both reads are lock-free heap reads under the sharing model (the
    //same contract as ThrownMessageOf).
    std::string typeName = "exception";
    if (m_currModule != nullptr) {
        try {
            typeName = LeafNameOfKey(HostClassName(excHeapIdx));
        } catch (const std::exception&) {
            //defensive: a malformed record still reports the hop
        }
    }
    std::string message;
    const StructSlot& slot = m_structHeap[static_cast<size_t>(excHeapIdx)];
    if (slot.size() > 1 && IsLiveStringHandle(slot[1]))
        FlattenInto(message, slot[1]);
    std::string chain = message.empty()
        ? typeName
        : typeName + ": " + message;
    return chain + FormatCauseChainTail(excHeapIdx);
}

//v0.12.4 D8: the "Caused by:" tail — one line per cause link, walking
//cell[5] while it addresses a live class slot. Bounds-checked per hop;
//cycles are impossible by construction (see the header).
std::string VmExecutor::FormatCauseChainTail(int32_t excHeapIdx) const {
    std::string tail;
    int32_t cause = 0;
    {
        const StructSlot& slot =
            m_structHeap[static_cast<size_t>(excHeapIdx)];
        if (slot.size() > 5)
            cause = slot[5];
    }
    while (cause > 0
           && static_cast<size_t>(cause) < m_structHeap.size()
           && m_slotKinds[static_cast<size_t>(cause)] == RTK_Class) {
        const StructSlot& slot = m_structHeap[static_cast<size_t>(cause)];
        std::string typeName = "exception";
        if (m_currModule != nullptr) {
            try {
                typeName = LeafNameOfKey(HostClassName(cause));
            } catch (const std::exception&) {
                break;   //malformed link: report the hops gathered so far
            }
        }
        std::string message;
        if (slot.size() > 1 && IsLiveStringHandle(slot[1]))
            FlattenInto(message, slot[1]);
        tail += "\nCaused by: ";
        tail += message.empty() ? typeName : typeName + ": " + message;
        cause = slot.size() > 5 ? slot[5] : 0;
    }
    return tail;
}

void VmExecutor::OpThrow(BytecodeReader& reader, uint8_t* locals) {
    //Throw arm keeps the store lock: FireOnThrow's debug checkpoint
    //(OnThrow + scheduler focus) is serialized only by it (OP_DebugInfo
    //is non-pure for the same reason); ThrownMessageOf itself is a
    //lock-free heap read under the sharing model.
    GilEnsureGuard g(*this);
    //Phase 9d: user `throw <expr>;` — read heap idx from src slot,
    //raise NLangThrow carrying the heap idx. Catch block above will
    //dispatch via func.tryBlocks.
    uint16_t src = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    //Debugger checkpoint at the throw site, before unwinding
    //starts (user throws do not pass through
    //RaiseNlangException — same checkpoint, shared helper).
    FireOnThrow();
    throw NLangThrow(heapIdx, ThrownMessageOf(heapIdx, "user throw"));
}

void VmExecutor::OpRethrow() {
    //Throw arm keeps the store lock: FireOnThrow's debug checkpoint
    //(OnThrow + scheduler focus) is serialized only by it (OP_DebugInfo
    //is non-pure for the same reason); ThrownMessageOf itself is a
    //lock-free heap read under the sharing model.
    GilEnsureGuard g(*this);
    //Phase 9d: `throw;` — re-raise the currently-caught exception.
    //Pops nothing; the matching OP_PopHandler at catch-block exit
    //(normal path) handles that.
    auto& s = EvalStack().back().handlerExcStack;
    if (s.empty())
        throw std::runtime_error(
            "NLang VM: rethrow outside catch handler");
    int32_t h = s.back();
    //Debugger checkpoint at the rethrow site (same contract as
    //OP_Throw / RaiseNlangException).
    FireOnThrow();
    throw NLangThrow(h, ThrownMessageOf(h, "rethrow"));
}

void VmExecutor::OpPopHandler() {
    //Phase 9d: emitted at the end of every catch body. Pops the
    //exception bound on catch entry so a later throw; in a different
    //scope doesn't accidentally use this frame's exception.
    auto& s = EvalStack().back().handlerExcStack;
    if (s.empty())
        throw std::runtime_error(
            "NLang VM: PopHandler underflow");
    s.pop_back();
}

} // namespace nlang
