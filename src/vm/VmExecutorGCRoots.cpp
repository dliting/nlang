/*---
    VmExecutorGCRoots.cpp — GC 根扫描：帧声明表（参数）、帧带位图与保守值测试。
    从 VmExecutorGC.cpp 拆出（2026-10-20 K2 slice 5，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>

namespace nlang {

//Root scan: one frame's declared locals. K2 slice 5: params only — the
//named locals moved into the band bitmap (MarkFrameBand below), which
//derives their classes from the same typeKind routing, so this scan
//kept its whole-table shape only while the mask path was bedded in.
//Keeping params here preserves their precise declared-kind scan for
//every mask state (the band starts after them).
void VmExecutor::MarkFrameLocals(const CallFrame& frame,
                                 std::vector<int32_t>& worklist) {
    for (auto& ld : frame.func->locals) {
        if (!ld.isParam)
            continue;
        if (ld.typeKind != RTK_Struct && ld.typeKind != RTK_Class
            && ld.typeKind != RTK_Array && ld.typeKind != RTK_Func
            && ld.typeKind != RTK_String && ld.typeKind != RTK_TypeParam)
            continue;
        int32_t val;
        std::memcpy(&val, frame.localsBuf.data() + ld.offset, sizeof(val));
        //String locals carry a string-store handle, not a heap index —
        //route before the heap-index guard below (a valid handle may
        //lie beyond the struct heap's size). MarkString does its own
        //validation (0 = the immortal empty string, harmlessly marked;
        //out-of-range skipped).
        if (ld.typeKind == RTK_String) {
            MarkString(val);
            continue;
        }
        //A T param's representation depends on the instantiation (store
        //handle, heap index or inline scalar) — test both domains; see
        //MarkFrameBand for the over-mark-only argument.
        if (ld.typeKind == RTK_TypeParam) {
            TestRootValue(val, worklist);
            continue;
        }
        if (val <= 0 || static_cast<size_t>(val) >= m_slotKinds.size())
            continue;
        if (!m_markBits[val])
            PushMarked(val, worklist);
    }
}

//Root scan: one frame's band [paramCount, localsSize), precise. The
//K2 slice 5 handle mask classifies every band slot (see EvalSlotClass):
//None skips the slot, StrHandle/HeapIndex test their domain only, Dual
//value-tests both. The band holds the return slot, the temp pool, the
//callParam staging area, the evalArea claims and the user locals —
//staged values with a safepoint in between are exactly the F2 shape
//(f(g(), h()) lost g's object while h allocated). A cross-domain hit
//can only over-mark (GC is non-moving), never under-mark, and unwritten
//cells are zero from frame initialization. Params are excluded: their
//kinds are declared and covered by the locals scan above.
//Fallback: a mask whose length disagrees with the frame layout (a
//hand-built in-memory record; the loader rejects the same shape at
//load) or the A/B test knob reverts to the conservative both-domain
//scan of the pre-mask era.
void VmExecutor::MarkFrameBand(const CallFrame& frame,
                               std::vector<int32_t>& worklist) {
    const CompiledFunction* func = frame.func;
    const size_t size = frame.localsBuf.size();
    const size_t paramSlots = func->paramCount;
    const size_t bandSlots = size / kFrameSlotBytes > paramSlots
        ? size / kFrameSlotBytes - paramSlots : 0;
    const bool maskUsable = !m_conservativeBandForTests
        && func->evalAreaHandleMask.size() == (bandSlots + 3) / 4;
    for (size_t i = 0; i < bandSlots; ++i) {
        const size_t off = (paramSlots + i) * kFrameSlotBytes;
        int32_t val;
        std::memcpy(&val, frame.localsBuf.data() + off, sizeof(val));
        if (!maskUsable) {
            TestRootValue(val, worklist);
            continue;
        }
        const unsigned cls =
            (func->evalAreaHandleMask[i >> 2] >> ((i & 3) * 2)) & 3u;
        if (cls == ESC_None)
            continue;
        if (cls & ESC_StrHandle) {
            if (val > 0
                    && static_cast<size_t>(val) < m_stringObjs.size())
                MarkString(val);
        }
        if (cls & ESC_HeapIndex) {
            if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
                    && !m_markBits[val])
                PushMarked(val, worklist);
        }
    }
}

//K2 slice 4: the conservative value test shared by the frame band scan
//and the staged native-call roots (VmExecutorGC.cpp). Both domains are
//tested; see MarkFrameBand for the over-mark-only argument.
void VmExecutor::TestRootValue(int32_t val,
                               std::vector<int32_t>& worklist) {
    if (val <= 0)
        return;
    if (static_cast<size_t>(val) < m_stringObjs.size())
        MarkString(val);
    if (static_cast<size_t>(val) < m_slotKinds.size() && !m_markBits[val])
        PushMarked(val, worklist);
}


//Root scan: static-slot cells (v0.12.6 Task 9). A static field or
//runtime constant outlives every frame that stored it — the slot cell
//is the only durable anchor once the setter's frame dies, exactly the
//shape MarkContextResultCell's accumulator happened to mask in the
//survival probe (a stale copy of the index kept the object marked; an
//in-churn scalar return displaced it and the sweep freed the object).
//Kind routing mirrors MarkFrameLocals: the slot's declared kind picks
//the domain (string handle vs heap idx), a T-typed slot tests both
//domains (over-mark only — the GC is non-moving). Cell writes happen
//only in bytecode handlers under the GIL; the collection runs at the
//quiescent rendezvous where no bytecode executes, so the read needs no
//lock — the same soundness argument as the parked-frame scans.
void VmExecutor::MarkStaticSlotRoots(std::vector<int32_t>& worklist) {
    const size_t count = m_staticSlots.Count();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t kind = m_currModule->staticSlots[i].type.kind;
        if (kind != RTK_Struct && kind != RTK_Class && kind != RTK_Array
            && kind != RTK_Func && kind != RTK_String
            && kind != RTK_TypeParam)
            continue;
        int32_t val;
        std::memcpy(&val, m_staticSlots.Cell(i), sizeof(val));
        if (kind == RTK_String) {
            MarkString(val);
            continue;
        }
        if (kind == RTK_TypeParam) {
            TestRootValue(val, worklist);
            continue;
        }
        if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
            && !m_markBits[val])
            PushMarked(val, worklist);
    }
}

//One context's resultCell root. K8 Task 7: for a LIVE context the
//cell is not just a return slot — PushFrameCore's result-cell
//inheritance makes it the accumulator every nested call in the
//context's root tree writes through. A parked channel recv's
//deliverer writes its boxed payload there while the owner is parked,
//and the [delivery, resume unbox] stretch spans a whole park, so a
//collection can complete with that handle anchored nowhere else (the
//conc_waitgroup ASan flake: swept slot, "unbox target is not a boxed
//primitive"). This scan runs at the quiescent rendezvous — the owner
//is arrived or parked, never mid-write — so the K7 torn-read
//objection (aimed at KIND-classified reads of running cells) does
//not apply; a conservative both-domain value test is sound and
//over-marks at worst (the same discipline as the frame band).
//K3: once the context has finished, its return value lives only in
//resultCell — classify it by the snapshotted return kind
//(MarkFrameResult's same split: string handle vs heap index) so a
//Future<T> of a reference T keeps the value alive between the spawn
//and the get(). K7 site 10: the discriminator is the context STATE,
//not frames.empty() — with the steal model kept intact, held and
//queued contexts have empty registry frames too.
void VmExecutor::MarkContextResultCell(const ExecutionContext* ctx,
                                       std::vector<int32_t>& worklist) {
    //Promise contexts: complete(T)'s value came through an erased
    //signature, so no resultKind classification exists (spawn's
    //returnTypeKind snapshot has no equivalent at a promise mint). The
    //conservative both-domain test covers every T; it over-marks at
    //worst (the live-context arm's discipline) and the scan runs at the
    //quiescent rendezvous, so no torn read.
    if (ctx->unclassifiedResult) {
        int32_t cellHalves[2];
        std::memcpy(cellHalves, &ctx->resultCell, sizeof(cellHalves));
        for (int32_t half : cellHalves)
            TestRootValue(half, worklist);
        return;
    }
    if (ctx->state != ContextState::Finished
            && ctx->state != ContextState::Failed) {
        int32_t cellHalves[2];
        std::memcpy(cellHalves, &ctx->resultCell, sizeof(cellHalves));
        for (int32_t half : cellHalves)
            TestRootValue(half, worklist);
        return;
    }
    uint8_t retKind = ctx->resultKind;
    int32_t val;
    std::memcpy(&val, &ctx->resultCell, sizeof(val));
    if (retKind == RTK_String) {
        MarkString(val);
    } else if (retKind == RTK_Class || retKind == RTK_Struct
        || retKind == RTK_Array || retKind == RTK_Func) {
        if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
            && !m_markBits[val])
            PushMarked(val, worklist);
    }
}

//K8 Task 7 product tripwire (the AssertAllArrived style): at the
//quiescent root scan, every pending delivery destination — a channel
//recv waiter's resultPtr, a parked get()'s joinResultDst — must fall
//on one of the scanned root surfaces: a frame buffer of either arm,
//or a registry resultCell (a call site inside a context's root tree
//accumulates through &ctx->resultCell — PushFrameCore's result-cell
//inheritance; the scan roots live cells conservatively). A
//destination escaping all of them means a frame-ownership transfer
//or a new accumulator shape left the delivery visible to no scan
//mid-flight — failing loudly here locates that bug at its source
//instead of a flaky downstream unbox error (the conc_waitgroup ASan
//flake's lesson). Cheap by construction: waiters are few and the
//range list is already built for the two scan arms.
void VmExecutor::AuditWaiterDestinationRanges(
    const std::vector<std::pair<const uint8_t*, const uint8_t*>>& ranges)
{
    auto covered = [&ranges](const uint8_t* dst) {
        for (const auto& r : ranges)
            if (dst >= r.first && dst < r.second)
                return true;
        return false;
    };
    auto isRegistryCell = [this](const uint8_t* dst) {
        for (size_t ci = 0; ci < m_spScheduler->ContextCount(); ++ci) {
            const ExecutionContext* ctx = m_spScheduler->Find(
                static_cast<int>(ci));
            if (ctx && dst
                    == reinterpret_cast<const uint8_t*>(&ctx->resultCell))
                return true;
        }
        return false;
    };
    for (size_t h = 0; h < m_channelStore.size(); ++h) {
        for (const auto& w : m_channelStore[h].waiters) {
            if (!covered(w.resultPtr) && !isRegistryCell(w.resultPtr)) {
                fprintf(stderr,
                    "NLang VM: channel waiter ctx=%d destination %p "
                    "escaped every GC root surface (frame transfer bug)\n",
                    w.contextId, w.resultPtr);
                std::terminate();
            }
        }
    }
    for (size_t ci = 0; ci < m_spScheduler->ContextCount(); ++ci) {
        const ExecutionContext* ctx = m_spScheduler->Find(
            static_cast<int>(ci));
        if (!ctx || !ctx->joinResultDst)
            continue;
        if (!covered(ctx->joinResultDst)
                && !isRegistryCell(ctx->joinResultDst)) {
            fprintf(stderr,
                "NLang VM: join result destination of ctx %d escaped "
                "every GC root surface (frame transfer bug)\n",
                static_cast<int>(ci));
            std::terminate();
        }
    }
}


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
            //Value form (RootMintedNativeHandle): the minted handle lives
            //only in the native's C++ local until the native writes it
            //into the staged ret cell, so the registry carries the value
            //itself. TestRootValue ignores the 0 of address-form entries.
            TestRootValue(root.value, worklist);
        }
    }
}

} //namespace nlang
