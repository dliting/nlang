/*---
    VmExecutorGC.cpp — 垃圾回收（安全点会合＋标记阶段）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）；
    容器元素追踪臂在 VmExecutorGCContainers.cpp（2026-10-20 拆出）；
    帧根扫描三件套在 VmExecutorGCRoots.cpp（2026-10-20 K2 slice 5 拆出）；
    清扫与释放族在 VmExecutorGCSweep.cpp（2026-10-24 K7 拆出）。
---*/
#include "VmExecutor.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace nlang {

//GC implementation.

//K7 sites 2/8: the real multiparty rendezvous. Poll gate: only windows
//at or below the innermost participation root poll (a window that flipped
//this thread's executing bit ON: carrier dispatch = depth 0, host
//outermost bridge entry, blocking-native callback re-entry). A raw
//"depth > 0" gate would silence host-outermost runs forever — they
//execute at depth >= 1 — and strand every stop that snapshots them
//executing. The generation guard ejects zombie carriers mid-bytecode
//(K6 excluded that shape by holding the GIL across every window).
//GIL-holding callers (spawn tail inside an intrinsic's store guard,
//guarded function entries) hand the lock off for the WHOLE sequence:
//the rendezvous contract parks on the cv holding no global lock, and
//K8's collection itself takes none — its epilogue touches only leaf
//arms (the quiescence gate, the root-registry iterations) plus one
//scheduler-domain guard (the registry scan).
void VmExecutor::CheckGCSafepoint() {
    if (m_spScheduler->CarrierRunGenerationStale())
        throw ShutdownSignal();   //zombie carrier from a torn-down run
    if (NestDepth() > GcPollRootDepth())
        return;   //suppressed above the innermost participation root —
                  //the window below polls on its own backedges
    GilHandoffScope handoff(*this);   //no-op unless the caller holds the GIL
    GcWorldStop& stop = m_spScheduler->GcStop();
    if (stop.StopInFlight()) {                  //somebody else initiated
        if (!stop.EndExecuting()) {             //Rule B: arrive, not last
            ++m_gcRendezvousParks;
            stop.WaitForCompletion();
            stop.BeginExecuting();   //Rule A — never under the global lock
        } else {
            CollectAndReenter(stop);   //K8: lock-free (quiescence-gated)
        }
        return;
    }
    if (!GcPressureExceeded())
        return;
    if (stop.InitiateAndArrive()) {
        //I'm (already) the last arriver. InitiateAndArrive flipped this
        //thread's executing bit off for the stop; bytecode continues
        //after the collection, so re-enter the denominator inside
        //CollectAndReenter.
        CollectAndReenter(stop);
    } else {
        ++m_gcRendezvousParks;
        stop.WaitForCompletion();
        stop.BeginExecuting();
    }
}

//The elected last arriver's collection plus its protocol tail: the
//caller's executing bit was flipped off by its arrival, so bytecode
//continuing afterwards must re-enter the denominator (BeginExecuting
//cannot block here — the stop completed — and no global lock is held:
//the safepoint's handoff released the GIL, the collection's own arms
//released theirs). If the collection throws, the re-entry still runs
//BEFORE the unwind continues: a window teardown that parks would call
//EndExecuting on an already-off bit (protocol tripwire), and skipping
//it leaves this thread outside the denominator mid-bytecode. The
//completion guard inside CollectAndComplete already completed the
//epoch, so the re-open cannot race a collector.
void VmExecutor::CollectAndReenter(GcWorldStop& stop) {
    try {
        CollectAndComplete();
    } catch (...) {
        stop.BeginExecuting();
        throw;
    }
    stop.BeginExecuting();   //Rule A — never under the global lock
}

//K8 site 2 (Task 5): the last arriver's epilogue — LOCK-FREE
//collection. The quiescence gate comes first: AssertAllArrived pins
//the constructive fact that every snapshot member is parked (no
//window can re-open mid-collection — Rule A waits out the epoch — so
//no bytecode and no store allocation can run; the mark and sweep
//arms touch the stores without any domain lock). The scheduler
//registry scan keeps the scheduler domain (timer-thread state flips
//— MarkSchedulerRoots), and the two root registries iterate under
//their leaf mutex (their mutators — blocking-native arms, host
//threads — sit outside the rendezvous denominator). Completion is
//RAII (the K7 plan's deferred robustness item): every other snapshot
//member is parked on this epoch, so an exception escaping the
//collection (mark worklist bad_alloc, the injected test failure)
//must still complete it before unwinding — otherwise the waiters
//strand forever. An interrupted sweep only leaks free slots until
//the retry (the pressure flag below never cleared), never
//double-pushes. On the normal path the guard's destructor completes
//AFTER the pressure-flag clear, so every joiner wakes to a cleared
//flag — no collection storm.
void VmExecutor::CollectAndComplete() {
    m_spScheduler->GcStop().AssertAllArrived();
    struct CompletionFirstGuard {
        GcWorldStop& stop;
        ~CompletionFirstGuard() { stop.Complete(); }
    };
    CompletionFirstGuard completeFirst{m_spScheduler->GcStop()};
    if (m_gcThrowOnceForTests.exchange(false, std::memory_order_relaxed))
        throw std::runtime_error("injected collection failure");
    CollectGarbage();
    m_gcPressure.store(false, std::memory_order_release);
}

//K7 site 2: the allocation sites' trigger store (every caller holds the
//GIL — the store-size reads are safe there; the backedge poll only
//loads the flag).
void VmExecutor::NoteGcPressure() {
    m_gcPressure.store(
        m_structHeap.size() > m_gcThreshold
            || m_stringObjs.size() > m_strGcThreshold,
        std::memory_order_release);
}

bool VmExecutor::GcPressureExceeded() const {
    return m_gcPressure.load(std::memory_order_acquire);
}

void VmExecutor::CollectGarbage() {
    ++m_gcCollections;   //K6 D7: per-run collection count
    MarkPhase();
    SweepPhase();
    //String sweep runs after the heap sweep, with marking completed for
    //both stores (one mark phase fills both bit vectors).
    SweepStrings();
}

//2026-09-26 maintainability split: MarkPhase was a 233-line method; the
//root scans and per-kind trace arms below are its decomposition (bodies
//verbatim modulo the extracted mark+enqueue step and guard-to-early-return
//conversions at the arm heads). PushMarked is that shared final step.
void VmExecutor::PushMarked(int32_t idx, std::vector<int32_t>& worklist) {
    m_markBits[idx] = true;
    worklist.push_back(idx);
}

void VmExecutor::MarkPhase() {
    m_markBits.assign(m_structHeap.size(), false);
    m_strMarkBits.assign(m_stringObjs.size(), false);
    //Identify root references from all call frames and push to worklist.
    //K7 site 10: the registry arm covers every registered thread's
    //published evaluation stack — all quiescent by the rendezvous
    //guarantee (arrived threads block in WaitForCompletion; others can
    //only touch their stack again through BeginExecuting's Rule A gate).
    //K8 Task 7: both frame-root arms and the transfer-freeze bracket
    //live in ScanFrameRootSurfaces (see its invariant comment).
    std::vector<int32_t> worklist;
    ScanFrameRootSurfaces(worklist);
    //K2 slice 4 (§2.5): staged blocking-native cells (argument copies,
    //return slot) and callback-minted handles — roots for as long as
    //their carrier's GIL is away.
    MarkNativeCallRoots(worklist);
    MarkHostRoots(worklist);
    //Iteratively trace references until worklist is empty.
    while (!worklist.empty()) {
        int32_t idx = worklist.back();
        worklist.pop_back();
        if (m_slotKinds[idx] == RTK_Func) {
            MarkFuncReceiver(idx, worklist);
        } else if (m_slotKinds[idx] == RTK_Boxed) {
            MarkBoxedPayload(idx);
        } else if (m_slotKinds[idx] == RTK_Class) {
            MarkClassFields(idx, worklist);
            MarkListElements(idx, worklist);
            MarkDictEntries(idx, worklist);
            MarkChannelElements(idx, worklist);
        } else if (m_slotKinds[idx] == RTK_Struct) {
            MarkStructFields(idx, worklist);
        } else if (m_slotKinds[idx] == RTK_Array) {
            MarkArrayElements(idx, worklist);
        }
    }
}

//K8 Task 7: the two frame-root arms run as ONE critical section
//against frame-ownership transfers. A context's frames sit on
//exactly one of two scan surfaces — the owning carrier slot's
//published vector (arm 1) or the registry ctx->frames (arm 2) —
//and every transfer between them (ParkContextFrames, RunContext-
//Window's restore, the blocking-native steal/restore pair) runs
//under the scheduler lock. Bracketing BOTH arms with that lock
//makes the pair indivisible: a stack parked after arm 1 is still
//in ctx->frames for arm 2, and a stack restored before arm 2 was
//already scanned from its slot by arm 1. Without the bracket a
//restore slipped between the arms escaped both scans — a parked
//recv's delivered handle then swept while its only anchor was in
//flight (the conc_waitgroup ASan flake). Scheduler→gcStop-leaf is
//the one legal order, so the nested leaf lock is fine, and
//MarkSchedulerRoots' own guard nests as a no-op.
void VmExecutor::ScanFrameRootSurfaces(std::vector<int32_t>& worklist) {
    VmScheduler::GilEnsureGuard rootScanTransferFreeze(*m_spScheduler);
    std::vector<std::pair<const uint8_t*, const uint8_t*>> frameRanges;
    m_spScheduler->GcStop().ScanRegisteredStacks(
        [&worklist, &frameRanges, this](std::vector<CallFrame>& stack) {
            for (CallFrame& frame : stack) {
                MarkFrameLocals(frame, worklist);
                MarkFrameResult(frame, worklist);
                MarkFrameBand(frame, worklist);
                const uint8_t* b = frame.localsBuf.data();
                frameRanges.emplace_back(b, b + frame.localsBuf.size());
            }
        });
    MarkSchedulerRoots(worklist);
    for (size_t ci = 0; ci < m_spScheduler->ContextCount(); ++ci) {
        const ExecutionContext* ctx = m_spScheduler->Find(
            static_cast<int>(ci));
        if (!ctx)
            continue;
        for (const CallFrame& frame : ctx->frames) {
            const uint8_t* b = frame.localsBuf.data();
            frameRanges.emplace_back(b, b + frame.localsBuf.size());
        }
    }
    AuditWaiterDestinationRanges(frameRanges);
}

//Extension point ③: host-registered roots (host-held reference Values).
//Same string/heap split as the frame scans — RTK_String values are
//string-store handles routed to MarkString (its own validation),
//everything else is a heap idx under the mark guard. K8: the registry
//iterates under its leaf mutex — host threads and blocking-native
//carriers mutate it from OUTSIDE the rendezvous denominator (their
//windows are suspended), so the collection's read needs the same
//mutex; MarkString/PushMarked take no lock, keeping this a leaf
//critical section.
void VmExecutor::MarkHostRoots(std::vector<int32_t>& worklist) {
    std::lock_guard<std::mutex> g(m_rootRegistryMutex);
    for (const HostRoot& root : m_hostRoots) {
        if (root.kind == RTK_String) {
            MarkString(root.value);
        } else if (root.value > 0
                && static_cast<size_t>(root.value) < m_slotKinds.size()
                && !m_markBits[root.value]) {
            PushMarked(root.value, worklist);
        }
    }
}

//K1 Slice 6 root arm: parked/queued contexts are not in EvalStack()
//(the steal model) — their whole frame stacks are roots here, plus each
//context's Thread object (join reads __handle) and a Failed context's
//exception object (join rethrows it by heap idx; it must survive until
//every joiner has thrown). Same three frame arms as the running stack;
//MarkFrameResult is safe on parked frames (externalResult points at
//the stable result cells). K8: one scheduler-domain guard over the
//whole scan — the collection itself is GIL-free, and the timer thread
//(also outside the rendezvous) flips ctx->state under the scheduler
//lock; the guard also replaces 2N+2 per-call self-acquires of
//Find/ContextCount. The mark helpers take no lock, so the guard holds
//nothing inward (GIL→gcStop-leaf is the one legal order and is not
//used here — ScanRegisteredStacks runs before this arm, un-nested).
void VmExecutor::MarkSchedulerRoots(std::vector<int32_t>& worklist) {
    VmScheduler::GilEnsureGuard gil(*m_spScheduler);
    for (size_t i = 0; i < m_spScheduler->ContextCount(); ++i) {
        const ExecutionContext* ctx = m_spScheduler->Find(
            static_cast<int>(i));
        for (const CallFrame& parkedFrame : ctx->frames) {
            MarkFrameLocals(parkedFrame, worklist);
            MarkFrameResult(parkedFrame, worklist);
            MarkFrameBand(parkedFrame, worklist);
        }
        const int32_t ctxRoots[3] = { ctx->threadObjHeapIdx,
                                      ctx->failExcHeapIdx,
                                      ctx->resumeExcHeapIdx };
        for (int32_t val : ctxRoots) {
            if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
                && !m_markBits[val])
                PushMarked(val, worklist);
        }
        MarkContextResultCell(ctx, worklist);
    }
}

//K2 slice 4 (§2.5 staged roots): in-flight blocking native calls. The
//address form covers the staged argument cells and the staged return
//cell — their contents exist only after entry (the ret cell is written
//by the native itself), so only an address can see the final values;
//argument cells are immutable past the entry copy (const in the ABI).
//The value form covers handles minted by re-entrant NativeHost
//callbacks during the blocking section. K8: registry mutations take
//the root-registry leaf mutex (K7's GIL serialization died with the
//lock-free allocation paths), and a blocking-native carrier sits
//outside the rendezvous denominator — the collection's iteration
//takes the same mutex. TestRootValue/PushMarked take no lock.
void VmExecutor::MarkNativeCallRoots(std::vector<int32_t>& worklist) {
    std::lock_guard<std::mutex> g(m_rootRegistryMutex);
    for (const NativeCallRoot& root : m_nativeCallRoots) {
        if (root.cells) {
            for (uint32_t i = 0; i < root.cellCount; ++i) {
                int32_t val;
                std::memcpy(&val,
                    root.cells + static_cast<size_t>(i) * kFrameSlotBytes,
                    sizeof(val));
                TestRootValue(val, worklist);
            }
        } else {
            TestRootValue(root.value, worklist);
        }
    }
}

//Root scan: one frame's result cell (same string-handle split as the
//locals scan above). FrameResultCell resolves the threading model
//(external C++ cell for root/toString/host windows, caller-frame slot
//for call-pushed frames); a null cell means the frame carries no
//result destination (defensive — never armed today).
void VmExecutor::MarkFrameResult(const CallFrame& frame,
                                 std::vector<int32_t>& worklist) {
    uint8_t* resultCell = FrameResultCell(frame);
    if (!resultCell)
        return;
    uint8_t retKind = frame.func->returnTypeKind;
    int32_t val;
    std::memcpy(&val, resultCell, sizeof(val));
    //String return slot: same handle-not-heap-index split as locals.
    //Deliberately conservative (adjudicated over-mark, spec D6
    //note): a pResult-resident string is also reachable from the
    //callee's own roots, so no source shape can pin this face red.
    //Kept as defense in depth — do not "tighten" it into an
    //under-mark.
    if (retKind == RTK_String) {
        MarkString(val);
    } else if (retKind == RTK_Class || retKind == RTK_Struct
        || retKind == RTK_Array || retKind == RTK_Func) {
        if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
            && !m_markBits[val]) {
            PushMarked(val, worklist);
        }
    }
}

//Phase 13: trace the captured receiver (slot[1]) of a bound
//handle; slot[0] is a function/name index, not a heap slot.
void VmExecutor::MarkFuncReceiver(int32_t idx,
                                  std::vector<int32_t>& worklist) {
    int32_t thisIdx = m_structHeap[idx][1];
    if (thisIdx > 0
        && static_cast<size_t>(thisIdx) < m_slotKinds.size()
        && m_slotKinds[static_cast<size_t>(thisIdx)] == RTK_Class
        && !m_markBits[static_cast<size_t>(thisIdx)]) {
        PushMarked(thisIdx, worklist);
    }
}

//String objectization: a boxed string's payload (slot[1]) is a
//string handle. Other boxed tags carry raw bits (no children).
void VmExecutor::MarkBoxedPayload(int32_t idx) {
    if (m_structHeap[idx][0] == RTK_String)
        MarkString(m_structHeap[idx][1]);
}

void VmExecutor::MarkClassFields(int32_t idx,
                                 std::vector<int32_t>& worklist) {
    int32_t classIdx = m_structHeap[idx][0];
    //0.9.12 T1: declared field kinds come from the layout view.
    const auto& fields = m_classLayout.Fields(static_cast<size_t>(classIdx));
    for (size_t i = 0; i < fields.size(); ++i) {
        int32_t refIdx = m_structHeap[idx][i * 2 + 1];
        //String fields carry a string-store handle, not a heap
        //index — route before the heap-index guard (a valid handle
        //may lie beyond the struct heap's size). No runtime slot-
        //kind double condition: the string store has no slotKinds.
        if (fields[i].kind == RTK_String) {
            MarkString(refIdx);
            continue;
        }
        if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
            continue;
        //Array redesign B: field kinds come from the declared type,
        //so RTK_Array fields are routed explicitly (same declared-
        //plus-runtime double condition as the reference kinds).
        //The old unconditional runtime-kind fallback is safe to drop:
        //jagged declaration forms that could smuggle an array record
        //into a non-array field slot are rejected at resolve time.
        //Pre-fix v1.9 modules also misfile array fields as
        //RTK_Int32 here; the v1.10 loader floor (same release)
        //refuses them, closing the GC under-trace window.
        //Object-declared fields (declared kind RTK_Class) also
        //hold boxed primitive records (runtime kind RTK_Boxed) —
        //push those too; the worklist's boxed arm marks a wrapped
        //string payload. Without this arm the sweep frees the
        //box while the field still points at it.
        if ((fields[i].kind == RTK_Class
                && (m_slotKinds[refIdx] == RTK_Class
                    || m_slotKinds[refIdx] == RTK_Boxed))
            || (fields[i].kind == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)
            || (fields[i].kind == RTK_Func && m_slotKinds[refIdx] == RTK_Func)
            || (fields[i].kind == RTK_Array && m_slotKinds[refIdx] == RTK_Array)) {
            if (!m_markBits[refIdx])
                PushMarked(refIdx, worklist);
        }
    }
}

void VmExecutor::MarkStructFields(int32_t idx,
                                  std::vector<int32_t>& worklist) {
    uint16_t structIdx = m_slotStructIdx[idx];
    auto& cs = m_currModule->structs[structIdx];
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        int32_t refIdx = m_structHeap[idx][i * 2];
        //String fields carry a string-store handle, not a heap
        //index — same pre-guard routing as the class field arm.
        if (cs.fieldTypeKinds[i] == RTK_String) {
            MarkString(refIdx);
            continue;
        }
        if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
            continue;
        //Explicit RTK_Array route (declared kind now authoritative) —
        //see the RTK_Class branch above for why the old runtime-kind
        //fallback is gone, including the v1.10 module-floor coupling.
        //Same Object-field boxed-record acceptance as the class
        //field arm above (an Object field inside a struct).
        if ((cs.fieldTypeKinds[i] == RTK_Class
                && (m_slotKinds[refIdx] == RTK_Class
                    || m_slotKinds[refIdx] == RTK_Boxed))
            || (cs.fieldTypeKinds[i] == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)
            || (cs.fieldTypeKinds[i] == RTK_Func && m_slotKinds[refIdx] == RTK_Func)
            || (cs.fieldTypeKinds[i] == RTK_Array && m_slotKinds[refIdx] == RTK_Array)) {
            if (!m_markBits[refIdx])
                PushMarked(refIdx, worklist);
        }
    }
}

namespace {
//Single source for the array-element trace acceptance (was four
//hand-expanded else-if arms): declared elemKind must match the record's
//runtime slot kind. Object[] elements carry boxed primitive records
//(declared RTK_Class, runtime RTK_Boxed) — same acceptance as the
//field arms. The RTK_Array row (an array whose elements are themselves
//array records, array redesign B spec §5.3#1) is currently unreachable
//from compilable source (jagged declarations rejected; List<int[]>
//elements live in the container store) — defensive base for
//future/external .nmod paths. String elements never reach this test
//(they carry store handles, routed before it).
inline bool ArrayElemKindAccepts(uint8_t elemKind, uint8_t runtimeKind)
{
    return (elemKind == RTK_Class
                && (runtimeKind == RTK_Class || runtimeKind == RTK_Boxed))
        || (elemKind == RTK_Struct && runtimeKind == RTK_Struct)
        || (elemKind == RTK_Func && runtimeKind == RTK_Func)
        || (elemKind == RTK_Array && runtimeKind == RTK_Array);
}
} // namespace

void VmExecutor::MarkArrayElements(int32_t idx,
                                   std::vector<int32_t>& worklist) {
    uint16_t arrayTypeIdx = m_slotStructIdx[idx];
    auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    int32_t length = m_structHeap[idx][2];
    //0.7.5: stride from the registry — reference-bearing kinds are all
    //1-cell (the acceptance test below unchanged); 2-cell scalars hold
    //no references, the honest offset just keeps the reads
    //element-aligned.
    const int cells = ArrayElemCells(at.elemKind);
    for (int32_t i = 0; i < length; ++i) {
        int32_t elemRef = m_structHeap[idx][3 + i * cells];
        //String elements are raw string handles (hole-1 uses 0 =
        //null) — same pre-guard routing as the field arms.
        if (at.elemKind == RTK_String) {
            MarkString(elemRef);
            continue;
        }
        if (elemRef <= 0 || static_cast<size_t>(elemRef) >= m_slotKinds.size())
            continue;
        if (ArrayElemKindAccepts(at.elemKind, m_slotKinds[elemRef])
                && !m_markBits[elemRef]) {
            PushMarked(elemRef, worklist);
        }
    }
}

} // namespace nlang
