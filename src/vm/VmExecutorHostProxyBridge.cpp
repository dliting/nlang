/*---
    VmExecutorHostProxyBridge.cpp — 读写代理面的桥（Task 9）。
    容器变更器（set/removeAt/clear/remove）与两个 kind 探针；与
    VmExecutorHostBridge.cpp 同契约：既有内部机制的薄包装，
    std::runtime_error 报坏索引/坏句柄（适配层翻译为 BadValue）。
    独立 TU：HostBridge 已近文件行数上限（500）。
---*/
#include "VmExecutor.h"
#include <stdexcept>

namespace nlang {

void VmExecutor::HostListSet(int32_t listHeapIdx, uint32_t index,
                             int32_t elemHeapIdx) {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(listHeapIdx)][kListHandleFieldOffset];
    auto& elements =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
    if (index >= elements.size())
        throw std::runtime_error("NLang VM: host list index out of bounds");
    elements[index] = elemHeapIdx;
}

void VmExecutor::HostListRemoveAt(int32_t listHeapIdx, uint32_t index) {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(listHeapIdx)][kListHandleFieldOffset];
    auto& elements =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
    if (index >= elements.size())
        throw std::runtime_error("NLang VM: host list index out of bounds");
    elements.erase(elements.begin() + static_cast<ptrdiff_t>(index));
}

void VmExecutor::HostListClear(int32_t listHeapIdx) {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(listHeapIdx)][kListHandleFieldOffset];
    m_listStore[static_cast<size_t>(handle) - 1].elements.clear();
}

void VmExecutor::HostDictRemove(int32_t dictHeapIdx, int32_t keyHeapIdx) {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(dictHeapIdx)][kListHandleFieldOffset];
    auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (HostKeysEqual(it->first, keyHeapIdx)) {
            entries.erase(it);
            return;
        }
    }
    //absent key: map-erase semantics — no-op
}

void VmExecutor::HostDictClear(int32_t dictHeapIdx) {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(dictHeapIdx)][kListHandleFieldOffset];
    m_dictStore[static_cast<size_t>(handle) - 1].entries.clear();
}

uint8_t VmExecutor::HostArrayElemKind(int32_t heapIdx) const {
    //Heap layout fact: [1]=arrayTypeIdx (VmExecutorAlloc.cpp:75-110).
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    return m_currModule->arrayTypes[
        static_cast<size_t>(slot[1])].elemKind;
}

uint8_t VmExecutor::HostSlotKind(int32_t heapIdx) const {
    if (heapIdx <= 0
            || static_cast<size_t>(heapIdx) >= m_slotKinds.size())
        throw std::runtime_error(
            "NLang VM: host slot kind on bad heap idx");
    return m_slotKinds[static_cast<size_t>(heapIdx)];
}

}  // namespace nlang
