/*---
    VmExecutorContainers.cpp — List/Dict 句柄与元素查找辅助
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
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
    //RTK_Boxed: the shared full-payload comparator (0.7.5: 8-byte rows
    //carry a hi cell a lo-only compare silently dropped).
    return BoxedValuesEqual(k1, k2);
}

bool VmExecutor::BoxedValuesEqual(int32_t a, int32_t b) const
{
    if (a <= 0 || b <= 0
        || static_cast<size_t>(a) >= m_structHeap.size()
        || static_cast<size_t>(b) >= m_structHeap.size())
        return false;
    const auto& recA = m_structHeap[static_cast<size_t>(a)];
    const auto& recB = m_structHeap[static_cast<size_t>(b)];
    if (recA[0] != recB[0])
        return false;
    if (recA[0] == RTK_String) {
        //Value cells are string-object handles. This method is const (a
        //GC/dict helper, never an execution path), so content comparison
        //goes through the non-mutating StrValCopy; invalid handles read "".
        return StrValCopy(recA[kBoxedValueSlot])
            == StrValCopy(recB[kBoxedValueSlot]);
    }
    int row = ScalarPrimIndexOfRtk(static_cast<uint8_t>(recA[0]));
    if (row < 0)
        return false;
    if (recA[kBoxedValueSlot] != recB[kBoxedValueSlot])
        return false;
    //8-byte rows (long/ulong) must compare the hi cell too; narrower
    //rows keep the value-extension convention in the lo cell alone.
    return kScalarPrims[row].slotWidth < 8
        || recA[kBoxedValueSlot + 1] == recB[kBoxedValueSlot + 1];
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
//against each stored element: boxed primitives through BoxedValuesEqual
//(strings by content, scalars by full payload — 0.7.5: 8-byte rows
//compare both cells) and reference values by identity. Returns the
//matching index or -1.
int32_t VmExecutor::FindListElement(const ListSlot& list, int32_t value)
{
    bool primitiveT = (value > 0
        && static_cast<size_t>(value) < m_slotKinds.size()
        && m_slotKinds[value] == RTK_Boxed);
    for (size_t i = 0; i < list.elements.size(); ++i) {
        int32_t elem = list.elements[i];
        bool match = false;
        if (primitiveT) {
            if (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && m_slotKinds[elem] == RTK_Boxed)
                match = BoxedValuesEqual(elem, value);
        } else {
            if (elem == value) match = true;
        }
        if (match) return static_cast<int32_t>(i);
    }
    return -1;
}

} // namespace nlang
