/*---
    VmExecutorGCContainers.cpp — GC 容器元素追踪臂：List/Dict/Channel
    实例持有的元素标记（从 VmExecutorGC.cpp 拆出，2026-10-20 尺寸守卫，
    零行为变化）。三者同形：classIdx 守卫 → __handle → 侧表 → 逐元素
    IsChildBearingHeapKind 过标。
---*/
#include "VmExecutor.h"

namespace nlang {

namespace {
//Single source for the container trace arms' kind set (was four
//hand-expanded literals: List/Dict x mark/push). At kind granularity the
//mark set and the worklist push set coincide — every traced kind has
//children for the dispatch below to walk (func's receiver, array
//elements, boxed payload, class/struct fields).
inline bool IsChildBearingHeapKind(uint8_t kind) {
    return kind == RTK_Class || kind == RTK_Struct || kind == RTK_Boxed
        || kind == RTK_Func || kind == RTK_Array;
}
} // namespace

//Phase 8e-3: trace List<T> elements as additional GC roots.
void VmExecutor::MarkListElements(int32_t idx,
                                  std::vector<int32_t>& worklist) {
    int32_t classIdx = m_structHeap[idx][0];
    if (static_cast<int16_t>(classIdx) != m_listClassIdx)
        return;
    int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
    if (handle <= 0)
        return;
    size_t h = static_cast<size_t>(handle - 1);
    if (h >= m_listStore.size())
        return;
    for (int32_t elem : m_listStore[h].elements) {
        if (elem > 0
            && static_cast<size_t>(elem) < m_slotKinds.size()
            && !m_markBits[elem]) {
            if (IsChildBearingHeapKind(m_slotKinds[elem]))
                PushMarked(elem, worklist);
        }
    }
}

//Phase 8e-4: trace Dict<K,V> entries (both K and V are heap idxs).
void VmExecutor::MarkDictEntries(int32_t idx,
                                 std::vector<int32_t>& worklist) {
    int32_t classIdx = m_structHeap[idx][0];
    if (static_cast<int16_t>(classIdx) != m_dictClassIdx)
        return;
    int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
    if (handle <= 0)
        return;
    size_t h = static_cast<size_t>(handle - 1);
    if (h >= m_dictStore.size())
        return;
    for (auto& kv : m_dictStore[h].entries) {
        for (int32_t elem : {kv.first, kv.second}) {
            if (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && !m_markBits[elem]) {
                if (IsChildBearingHeapKind(m_slotKinds[elem]))
                    PushMarked(elem, worklist);
            }
        }
    }
}

//K1 Slice 9: trace Channel<T> buffered elements (heap idxs in FIFO
//order — boxed primitives or class refs). Waiters carry no heap idxs
// (raw result pointers only), so the buffer is the only trace arm.
void VmExecutor::MarkChannelElements(int32_t idx,
                                     std::vector<int32_t>& worklist) {
    int32_t classIdx = m_structHeap[idx][0];
    if (static_cast<int16_t>(classIdx) != m_channelClassIdx)
        return;
    int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
    if (handle <= 0)
        return;
    size_t h = static_cast<size_t>(handle - 1);
    if (h >= m_channelStore.size())
        return;
    for (int32_t elem : m_channelStore[h].buffered) {
        if (elem > 0
            && static_cast<size_t>(elem) < m_slotKinds.size()
            && !m_markBits[elem]) {
            if (IsChildBearingHeapKind(m_slotKinds[elem]))
                PushMarked(elem, worklist);
        }
    }
}

} // namespace nlang
