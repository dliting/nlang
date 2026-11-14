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
//K8: read-side snapshots for formatting faces — one Container-domain
//critical section each; callers format the copy without holding the
//guard (a user toString override may re-enter any domain).
std::vector<int32_t> VmExecutor::SnapshotListElements(
    int32_t handle) const {
    DomainGuard g(m_domainLocks, DomainId::Container);
    return m_listStore[static_cast<size_t>(handle) - 1].elements;
}

std::vector<std::pair<int32_t, int32_t>>
VmExecutor::SnapshotDictEntries(int32_t handle) const {
    DomainGuard g(m_domainLocks, DomainId::Container);
    return m_dictStore[static_cast<size_t>(handle) - 1].entries;
}

//K8: the allocators own the Container-domain guard — the side tables
//are plain vectors, so free-list pops and growth must be serialized
//against element writes. Reentrant for callers already inside the
//domain (intrinsic arms); the host bridge reaches here unguarded.
int32_t VmExecutor::AllocListHandle() {
    DomainGuard g(m_domainLocks, DomainId::Container);
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
    DomainGuard g(m_domainLocks, DomainId::Container);
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
    if (kind != RTK_Boxed && kind != RTK_Class && kind != RTK_Struct
        && kind != RTK_Func)
        return false;
    if (kind == RTK_Class)
        return k1 == k2;  //identity (k1!=k2 already checked above → false)
    //v0.13.0 D1: struct keys carry value semantics (every boundary deep
    //copies), so identity could never match — compare by field value.
    if (kind == RTK_Struct)
        return StructValuesEqual(k1, k2);
    //v0.13.0 D4: func keys compare by the handle-content triple —
    //OpMakeFunc never interns, so two references to one function are
    //distinct records with one identity.
    if (kind == RTK_Func)
        return FuncRecordValuesEqual(k1, k2);
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

bool VmExecutor::StructValuesEqual(int32_t a, int32_t b) const
{
    //Identity fast path; 0 is the null sentinel and equals only itself.
    if (a == b) return true;
    if (a <= 0 || b <= 0
        || static_cast<size_t>(a) >= m_structHeap.size()
        || static_cast<size_t>(b) >= m_structHeap.size())
        return false;
    if (m_slotKinds[static_cast<size_t>(a)] != RTK_Struct
        || m_slotKinds[static_cast<size_t>(b)] != RTK_Struct)
        return false;
    //Different struct types are different values.
    const uint16_t typeIdx = m_slotStructIdx[static_cast<size_t>(a)];
    if (typeIdx != m_slotStructIdx[static_cast<size_t>(b)]
        || typeIdx >= m_currModule->structs.size())
        return false;
    const auto& cs = m_currModule->structs[typeIdx];
    const auto& recA = m_structHeap[static_cast<size_t>(a)];
    const auto& recB = m_structHeap[static_cast<size_t>(b)];
    //Field i lives at cell 2i (uniform stride). The walk mirrors
    //DeepCopyStruct's member rule — its recursion arm here recurses,
    //its verbatim-copy arms compare handles.
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        const uint8_t kind = cs.fieldTypeKinds[i];
        const int32_t cell = i * 2;
        if (kind == RTK_Struct && cs.fieldStructIndices[i] != 0xFFFF) {
            if (!StructValuesEqual(recA[cell], recB[cell]))
                return false;
        } else if (kind == RTK_String) {
            if (StrValCopy(recA[cell]) != StrValCopy(recB[cell]))
                return false;
        } else if (int row = ScalarPrimIndexOfRtk(kind); row >= 0) {
            //Narrow kinds keep the value-extension convention in the lo
            //cell; 8-byte rows carry a hi cell (BoxedValuesEqual rule).
            if (recA[cell] != recB[cell]) return false;
            if (kScalarPrims[row].slotWidth >= 8
                && recA[cell + 1] != recB[cell + 1]) return false;
        } else {
            //Reference-shaped fields (class/array/interface/func): the
            //copy path stores the handle verbatim, so equality is the
            //handle — the dual of the shallow-copy arm.
            if (recA[cell] != recB[cell]) return false;
        }
    }
    return true;
}

bool VmExecutor::FuncRecordValuesEqual(int32_t a, int32_t b) const
{
    if (a <= 0 || b <= 0
        || static_cast<size_t>(a) >= m_structHeap.size()
        || static_cast<size_t>(b) >= m_structHeap.size())
        return false;
    //The record is {target, receiver, form} in cells 0..2 (OpMakeFunc
    //and the bound/virtual mints); OpFuncEquality compares exactly
    //these three cells.
    const auto& recA = m_structHeap[static_cast<size_t>(a)];
    const auto& recB = m_structHeap[static_cast<size_t>(b)];
    return recA[0] == recB[0] && recA[1] == recB[1] && recA[2] == recB[2];
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
//compare both cells), struct values through StructValuesEqual (v0.13.0
//D1: every boundary deep-copies, so identity never matched), func
//values through FuncRecordValuesEqual (v0.13.0 D4: references are not
//interned), and class/reference values by identity. Returns the
//matching index or -1.
int32_t VmExecutor::FindListElement(const ListSlot& list, int32_t value)
{
    bool primitiveT = (value > 0
        && static_cast<size_t>(value) < m_slotKinds.size()
        && m_slotKinds[value] == RTK_Boxed);
    bool structT = (value > 0
        && static_cast<size_t>(value) < m_slotKinds.size()
        && m_slotKinds[value] == RTK_Struct);
    bool funcT = (value > 0
        && static_cast<size_t>(value) < m_slotKinds.size()
        && m_slotKinds[value] == RTK_Func);
    for (size_t i = 0; i < list.elements.size(); ++i) {
        int32_t elem = list.elements[i];
        bool match = false;
        if (primitiveT) {
            if (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && m_slotKinds[elem] == RTK_Boxed)
                match = BoxedValuesEqual(elem, value);
        } else if (structT) {
            match = (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && m_slotKinds[elem] == RTK_Struct)
                && StructValuesEqual(elem, value);
        } else if (funcT) {
            match = (elem > 0
                && static_cast<size_t>(elem) < m_slotKinds.size()
                && m_slotKinds[elem] == RTK_Func)
                && FuncRecordValuesEqual(elem, value);
        } else {
            if (elem == value) match = true;
        }
        if (match) return static_cast<int32_t>(i);
    }
    return -1;
}

} // namespace nlang
