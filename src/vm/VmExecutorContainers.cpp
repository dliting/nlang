/*---
    VmExecutorContainers.cpp — List/Dict 句柄与元素查找辅助
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

//Stub implementations — will be filled in Step 7.

int32_t VmExecutor::AllocByteStreamHandle() {
    if (!m_byteStreamFreeList.empty()) {
        int32_t h = m_byteStreamFreeList.back();
        m_byteStreamFreeList.pop_back();
        m_byteStreams[static_cast<size_t>(h) - 1] = std::make_unique<ByteStreamState>();
        return h;
    }
    int32_t h = static_cast<int32_t>(m_byteStreams.size()) + 1;
    m_byteStreams.push_back(std::make_unique<ByteStreamState>());
    return h;
}

int32_t VmExecutor::AllocFileStreamHandle() {
    if (!m_fileStreamFreeList.empty()) {
        int32_t h = m_fileStreamFreeList.back();
        m_fileStreamFreeList.pop_back();
        m_fileStreams[static_cast<size_t>(h) - 1] = std::make_unique<FileStreamState>();
        return h;
    }
    int32_t h = static_cast<int32_t>(m_fileStreams.size()) + 1;
    m_fileStreams.push_back(std::make_unique<FileStreamState>());
    return h;
}

//Phase 8e-3: allocate a handle from the List<T> side table.
//Handles are 1-based; __handle==0 means null (uninitialized).
int32_t VmExecutor::AllocListHandle() {
    if (!m_listFreeList.empty()) {
        int32_t h = m_listFreeList.back();
        m_listFreeList.pop_back();
        m_listStore[h - 1] = ListSlot{};
        return h;
    }
    int32_t h = static_cast<int32_t>(m_listStore.size()) + 1;
    m_listStore.emplace_back();
    return h;
}

//Phase 8e-4: Dict<K,V> helpers — mirror List's shape.
int32_t VmExecutor::AllocDictHandle() {
    if (!m_dictFreeList.empty()) {
        int32_t h = m_dictFreeList.back();
        m_dictFreeList.pop_back();
        m_dictStore[h - 1] = DictSlot{};
        return h;
    }
    int32_t h = static_cast<int32_t>(m_dictStore.size()) + 1;
    m_dictStore.emplace_back();
    return h;
}

int32_t VmExecutor::ReadDictHandle(uint16_t callParamBase, uint8_t* locals,
    const char* methodName)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0)
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName + " on null instance");
    if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName + " on stale reference");
    int32_t handle = m_structHeap[static_cast<size_t>(thisHeapIdx)]
        [kListHandleFieldOffset];
    if (handle <= 0)
        throw std::runtime_error(
            std::string("NLang VM: Dict ") + methodName +
            " on uninitialized instance");
    return handle;
}

bool VmExecutor::DictKeysEqual(int32_t k1, int32_t k2) const {
    //Identity fast path (also handles k1==k2==0/null).
    if (k1 == k2) return true;
    if (k1 <= 0 || k2 <= 0) return false;
    if (static_cast<size_t>(k1) >= m_slotKinds.size()
        || static_cast<size_t>(k2) >= m_slotKinds.size())
        return false;
    //Kind must match.
    if (m_slotKinds[k1] != m_slotKinds[k2]) return false;
    auto kind = m_slotKinds[k1];
    if (kind != RTK_Boxed && kind != RTK_Class && kind != RTK_Struct)
        return false;
    if (kind == RTK_Class || kind == RTK_Struct)
        return k1 == k2;  //identity (k1!=k2 already checked above → false)
    //RTK_Boxed: branch on the wrapped type tag (slot[0]).
    int32_t tag1 = m_structHeap[static_cast<size_t>(k1)][0];
    int32_t tag2 = m_structHeap[static_cast<size_t>(k2)][0];
    if (tag1 != tag2) return false;
    int32_t bits1 = m_structHeap[static_cast<size_t>(k1)][kBoxedValueSlot];
    int32_t bits2 = m_structHeap[static_cast<size_t>(k2)][kBoxedValueSlot];
    if (tag1 == RTK_String) {
        //bits are string-object handles. This method is const (a GC/dict
        //helper, never an execution path), so content comparison goes
        //through the non-mutating StrValCopy; invalid handles read "".
        return StrValCopy(bits1) == StrValCopy(bits2);
    }
    return bits1 == bits2;  //int / float value bits
}

//Phase 8e-3 fix-up: read this.__handle from callParamBase[0] for a List
//intrinsic. Validates this-heap-idx, upper bound, and handle. Throws uniformly
//on null/stale/uninitialized; the previous "silently no-op for some methods"
//behavior was inconsistent (H1) and made bugs hard to spot.
int32_t VmExecutor::ReadListHandle(uint16_t callParamBase, uint8_t* locals,
    const char* methodName)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0)
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName + " on null instance");
    if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName + " on stale reference");
    int32_t handle = m_structHeap[static_cast<size_t>(thisHeapIdx)]
        [kListHandleFieldOffset];
    if (handle <= 0)
        throw std::runtime_error(
            std::string("NLang VM: List ") + methodName +
            " on uninitialized instance");
    return handle;
}

//Shared matcher behind IndexOf/Contains (was ~25 verbatim-duplicated
//lines in each intrinsic). Decodes the probe value, then compares it
//against each stored element: boxed primitives by tag — strings by
//content (aligns with == and Dict; the old payload bit compare made
//indexOf("hel"+"lo") miss a stored "hello"), int/float by value bits —
//and reference values by identity. Returns the matching index or -1.
int32_t VmExecutor::FindListElement(const ListSlot& list, int32_t value)
{
    bool primitiveT = (value > 0
        && static_cast<size_t>(value) < m_slotKinds.size()
        && m_slotKinds[value] == RTK_Boxed);
    int32_t valTag = 0, valBits = 0;
    if (primitiveT) {
        valTag = m_structHeap[static_cast<size_t>(value)][0];
        valBits = m_structHeap[static_cast<size_t>(value)][kBoxedValueSlot];
    }
    for (size_t i = 0; i < list.elements.size(); ++i) {
        int32_t elem = list.elements[i];
        bool match = false;
        if (primitiveT) {
            if (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && m_slotKinds[elem] == RTK_Boxed) {
                int32_t elemTag =
                    m_structHeap[static_cast<size_t>(elem)][0];
                int32_t elemBits =
                    m_structHeap[static_cast<size_t>(elem)][kBoxedValueSlot];
                if (elemTag == RTK_String && valTag == RTK_String)
                    match = (StrVal(elemBits) == StrVal(valBits));
                else
                    match = (elemBits == valBits);  //int/float bits
            }
        } else {
            if (elem == value) match = true;
        }
        if (match) return static_cast<int32_t>(i);
    }
    return -1;
}

} // namespace nlang
