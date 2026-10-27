/*---
    VmExecutorGCSweep.cpp — 垃圾回收的清扫与释放族
    从 VmExecutorGC.cpp 抽取（2026-10-24 K7 拆出，零行为变化）：
    SweepPhase 阈值回退、死亡类实例的容器句柄回收、嵌套 struct 与
    数组 struct 元素的递归释放、活跃槽计数。
---*/
#include "VmExecutor.h"

#include <algorithm>

namespace nlang {

void VmExecutor::SweepPhase() {
    //K8: sweep runs quiescent at the GC rendezvous — the one writer
    //allowed to touch every shard. ClearAll, then redistribute ALL free
    //indices (newly dead AND residual frees) by the block mapping. The
    //old clear-and-skip loop silently dropped residual frees at every
    //collection (monotone growth leak).
    m_heapShards.ClearAll();
    size_t liveSlots = 0;
    for (size_t i = 1; i < m_structHeap.size(); ++i) {
        if (m_slotKinds[i] == 0) {
            HeapSweepPush(static_cast<int32_t>(i));   //residual free stays free
            continue;
        }
        if (!m_markBits[i]) {
            if (m_slotKinds[i] == RTK_Class) {
                RecycleClassContainerHandles(i);
                FreeOwnedStructs(static_cast<int32_t>(i));
            }
            if (m_slotKinds[i] == RTK_Struct)
                FreeNestedStructs(static_cast<int32_t>(i), m_slotStructIdx[i]);
            if (m_slotKinds[i] == RTK_Array)
                FreeOwnedArrayStructElements(static_cast<int32_t>(i));
            m_structHeap[i].clear();
            m_slotKinds[i] = 0;
            HeapSweepPush(static_cast<int32_t>(i));
        } else {
            ++liveSlots;
        }
    }
    //K6 D3: the store never shrinks (free slots recycle in place), so the
    //trigger must stay above the high-water size after every sweep —
    //otherwise a small-live churn workload collects at every safepoint
    //forever. The size floor alone would collect on every growth
    //allocation once the free list empties (quadratic over a growing
    //live set); twice the live population adds geometric spacing there
    //(linear total cost). The larger of the two: drop-churn keeps the
    //predicate false until the free list empties, growth spends its
    //cadence geometrically. Stress clamps only the initial thresholds —
    //the backoff runs unconditionally (pre-K6 stress semantics).
    m_gcThreshold = std::max(m_gcThreshold,
        std::max(m_structHeap.size(), 2 * liveSlots));
}

//Dead class instance's container-handle recycling (the sweep's RTK_Class
//arm): List/Dict (Phase 8e-3/8e-4), Mutex/Channel (K1 Slices 8/9) and
//Semaphore (P2-1). Dropped-while-held instances of the sync primitives
//leave their waiters parked forever — the carrier's exit guard reports
//that shape.
void VmExecutor::RecycleClassContainerHandles(size_t i) {
    int32_t classIdx = m_structHeap[i][0];
    //Phase 8e-3: recycle List<T> handle when a List instance is freed.
    if (static_cast<int16_t>(classIdx) == m_listClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_listFreeList.push_back(handle);
    }
    //Phase 8e-4: recycle Dict<K,V> handle when a Dict instance is freed.
    if (static_cast<int16_t>(classIdx) == m_dictClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_dictFreeList.push_back(handle);
    }
    //K1 Slice 8: recycle Mutex handle when an instance is freed.
    if (static_cast<int16_t>(classIdx) == m_mutexClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_mutexFreeList.push_back(handle);
    }
    //K1 Slice 9: recycle Channel handle when an instance is freed
    //(same parked-waiter hazard and exit-guard report as a Mutex).
    if (static_cast<int16_t>(classIdx) == m_channelClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_channelFreeList.push_back(handle);
    }
    //P2-1 (v0.10.17): recycle Semaphore handle when an instance is
    //freed (same parked-waiter hazard and exit-guard report).
    if (static_cast<int16_t>(classIdx) == m_semaphoreClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_semaphoreFreeList.push_back(handle);
    }
    //0.10.21: recycle ReentrantMutex handle when an instance is freed
    //(same parked-waiter hazard and exit-guard report).
    if (static_cast<int16_t>(classIdx) == m_rmutexClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_rmutexFreeList.push_back(handle);
    }
    //0.10.22: recycle AtomicInt handle when an instance is freed (the
    //value is a plain int — no rooting, same recycling discipline).
    if (static_cast<int16_t>(classIdx) == m_atomicClassIdx) {
        int32_t handle = m_structHeap[i][kListHandleFieldOffset];
        if (handle > 0)
            m_atomicFreeList.push_back(handle);
    }
}

void VmExecutor::FreeOwnedStructs(int32_t heapIdx) {
    int32_t classIdx = m_structHeap[heapIdx][0];
    //0.9.12 T1: declared field kinds come from the layout view (the GC
    //trusts the cell-0 classIdx, same as MarkClassFields).
    const auto& fields = m_classLayout.Fields(static_cast<size_t>(classIdx));
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].kind == RTK_Struct && fields[i].structIdx != 0xFFFF) {
            int32_t structSlotIdx = m_structHeap[heapIdx][i * 2 + 1];
            if (structSlotIdx > 0 && static_cast<size_t>(structSlotIdx) < m_slotKinds.size()
                && m_slotKinds[structSlotIdx] == RTK_Struct) {
                FreeNestedStructs(structSlotIdx, fields[i].structIdx);
                m_structHeap[structSlotIdx].clear();
                m_slotKinds[structSlotIdx] = 0;
                HeapSweepPush(structSlotIdx);
            }
        }
    }
}

void VmExecutor::FreeNestedStructs(int32_t heapIdx, uint16_t structIdx) {
    auto& cs = m_currModule->structs[structIdx];
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t nestedIdx = m_structHeap[heapIdx][i * 2];
            if (nestedIdx > 0 && static_cast<size_t>(nestedIdx) < m_slotKinds.size()
                && m_slotKinds[nestedIdx] == RTK_Struct) {
                FreeNestedStructs(nestedIdx, cs.fieldStructIndices[i]);
                m_structHeap[nestedIdx].clear();
                m_slotKinds[nestedIdx] = 0;
                HeapSweepPush(nestedIdx);
            }
        }
    }
}

void VmExecutor::FreeOwnedArrayStructElements(int32_t heapIdx) {
    uint16_t arrayTypeIdx = m_slotStructIdx[heapIdx];
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        return;
    auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    if (at.elemKind != RTK_Struct)
        return;
    int32_t length = m_structHeap[heapIdx][2];
    for (int32_t i = 0; i < length; ++i) {
        int32_t elemRef = m_structHeap[heapIdx][3 + i];
        if (elemRef > 0 && static_cast<size_t>(elemRef) < m_slotKinds.size()
            && m_slotKinds[elemRef] == RTK_Struct) {
            FreeNestedStructs(elemRef, at.elemTypeIdx);
            m_structHeap[elemRef].clear();
            m_slotKinds[elemRef] = 0;
            HeapSweepPush(elemRef);
        }
    }
}

//K6 D7: live heap slots (kinds != free) — the heap-side counterpart of
//LiveStringObjectCount for bounded-memory assertions.
size_t VmExecutor::LiveHeapSlotCount() const {
    size_t live = 0;
    const size_t slotCount = m_slotKinds.size();
    for (size_t i = 0; i < slotCount; ++i)
        if (m_slotKinds[i] != 0) ++live;
    return live;
}

} // namespace nlang
