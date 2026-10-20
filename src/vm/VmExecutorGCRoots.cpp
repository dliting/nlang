/*---
    VmExecutorGCRoots.cpp — GC 根扫描：帧声明表（参数）、帧带位图与保守值测试。
    从 VmExecutorGC.cpp 拆出（2026-10-20 K2 slice 5，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>

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
            && ld.typeKind != RTK_String)
            continue;
        int32_t val;
        std::memcpy(&val, frame.localsBuf.data() + ld.offset, sizeof(val));
        //String locals carry a string-store handle, not a heap index —
        //route before the heap-index guard below (a valid handle may
        //lie beyond the struct heap's size). MarkString does its own
        //validation (0 = null, out-of-range skipped).
        if (ld.typeKind == RTK_String) {
            MarkString(val);
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

} //namespace nlang
