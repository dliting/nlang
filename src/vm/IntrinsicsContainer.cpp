/*---
    IntrinsicsContainer.cpp — List/Dict intrinsic family (K8).
    Moved verbatim from ExecuteIntrinsic (VmExecutorIntrinsics.cpp):
    structural arms run under one Container-domain guard; the
    toString arms snapshot-and-release inside FormatList/FormatDict,
    so a user toString override re-enters any domain without
    ordering against a held guard.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//K8: DeferredNlangRaise moved to VmExecutor.h (shared with the Sync
//family) — same contract: thrown by structural arms inside the domain
//guard, converted at the section boundary.

bool VmExecutor::ExecuteIntrinsicContainer(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //INTR_List_toString (Phase 9b-pre): format the list's elements as
    //"[e1, e2, ...]" via FormatList. Mint a string object, write its
    //handle to pResult. Strings inside are quoted via QuoteString.
    if (intrinsicId == INTR_List_toString) {
        int32_t handle = ReadListHandle(callParamBase, locals, "toString");
        std::string s = FormatList(handle, 0);
        int32_t strHandle = MintNewString(std::move(s));
        std::memcpy(pResult, &strHandle, sizeof(strHandle));
        return true;
    }
    //INTR_Dict_toString (Phase 9b-pre): format the dict's entries as
    //"{k1: v1, k2: v2, ...}" via FormatDict.
    if (intrinsicId == INTR_Dict_toString) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "toString");
        std::string s = FormatDict(handle, 0);
        int32_t strHandle = MintNewString(std::move(s));
        std::memcpy(pResult, &strHandle, sizeof(strHandle));
        return true;
    }


    DeferredNlangRaise deferred;
    {
        DomainGuard g(m_domainLocks, DomainId::Container);
        try {
            return ExecuteIntrinsicContainerStructural(
                intrinsicId, callParamBase, locals, pResult);
        } catch (DeferredNlangRaise& r) {
            deferred = std::move(r);
        }
    }
    RaiseNlangException(deferred.classIdx, deferred.message);
}

//Everything below the toString arms: one contiguous Container-domain
//critical section (handle resolution, structure mutation, growth —
//AllocListHandle/AllocDictHandle grow the side-table vectors here;
//inner allocations/mints are forward acquisitions).
bool VmExecutor::ExecuteIntrinsicContainerStructural(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //Phase 8e-3: List<T> built-in generic (erasure-style, 9 intrinsics).
    //All elements are heap idxs — boxed primitives or class refs. The same
    //CompiledClass "List" serves all instantiations. T-typed args are boxed
    //by VmBackend before OP_CallMethod; Get() returns boxed and is unboxed
    //by OP_Unbox after.

    //INTR_List_Ctor: allocate a ListSlot, store handle in __handle field.
    if (intrinsicId == INTR_List_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error("NLang VM: List ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: List ctor on stale reference");
        int32_t handle = AllocListHandle();
        //__handle is field slot kListHandleFieldOffset (slot 0 = classIdx)
        m_structHeap[static_cast<size_t>(thisHeapIdx)]
            [kListHandleFieldOffset] = handle;
        return true;
    }
    //INTR_List_Add: push element to list.
    if (intrinsicId == INTR_List_Add) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + kFrameSlotBytes, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "add");
        m_listStore[handle - 1].elements.push_back(value);
        return true;
    }
    //INTR_List_Get: return element at index.
    if (intrinsicId == INTR_List_Get) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + kFrameSlotBytes, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "get");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw DeferredNlangRaise{m_oobExcClassIdx,
                                     "NLang VM: List index out of bounds"};
        int32_t val = lst.elements[idx];
        std::memcpy(pResult, &val, sizeof(val));
        return true;
    }
    //INTR_List_Set: replace element at index.
    if (intrinsicId == INTR_List_Set) {
        int32_t idx, value;
        std::memcpy(&idx, locals + callParamBase + kFrameSlotBytes, sizeof(idx));
        std::memcpy(&value, locals + callParamBase + 2 * kFrameSlotBytes, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "set");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw DeferredNlangRaise{m_oobExcClassIdx,
                                     "NLang VM: List index out of bounds"};
        lst.elements[idx] = value;
        return true;
    }
    //INTR_List_Length: return elements.size().
    if (intrinsicId == INTR_List_Length) {
        int32_t handle = ReadListHandle(callParamBase, locals, "length");
        int32_t len = static_cast<int32_t>(m_listStore[handle - 1].elements.size());
        std::memcpy(pResult, &len, sizeof(len));
        return true;
    }
    //INTR_List_RemoveAt: erase element at index.
    if (intrinsicId == INTR_List_RemoveAt) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + kFrameSlotBytes, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "removeAt");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            throw DeferredNlangRaise{m_oobExcClassIdx,
                                     "NLang VM: List index out of bounds"};
        lst.elements.erase(lst.elements.begin() + idx);
        return true;
    }
    //INTR_List_IndexOf: linear search; return position or -1.
    //C2 fix: for primitive-T lists, OP_Box wraps `value` before this call,
    //so `value` is a heap idx into a RTK_Boxed slot. Compare slot[1] (value
    //bits). For class-T lists, `value` is the heap idx directly. Decide by
    //peeking at m_slotKinds of the first element (or of `value` itself when
    //the list is empty — caller-side OP_Box still allocated a slot we can
    //inspect).
    if (intrinsicId == INTR_List_IndexOf) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + kFrameSlotBytes, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "indexOf");
        int32_t result = FindListElement(m_listStore[handle - 1], value);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    //INTR_List_Contains: return 1 if found, 0 otherwise. Same primitive-vs-
    //class branching as IndexOf (C2 fix); matching lives in FindListElement.
    if (intrinsicId == INTR_List_Contains) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + kFrameSlotBytes, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "contains");
        int32_t result =
            (FindListElement(m_listStore[handle - 1], value) >= 0) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    //INTR_List_Clear: remove all elements.
    if (intrinsicId == INTR_List_Clear) {
        int32_t handle = ReadListHandle(callParamBase, locals, "clear");
        m_listStore[handle - 1].elements.clear();
        return true;
    }

    //Phase 8e-4: Dict<K,V> built-in generic (erasure-style, 7 intrinsics).
    //Keys and values are uniformly heap idxs (boxed primitives via OP_Box at
    //the call site, or class refs directly). Linear-scan lookup with kind-
    //aware equality via DictKeysEqual.

    //INTR_Dict_Ctor: allocate DictSlot, store handle.
    if (intrinsicId == INTR_Dict_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error("NLang VM: Dict ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: Dict ctor on stale reference");
        int32_t handle = AllocDictHandle();
        m_structHeap[static_cast<size_t>(thisHeapIdx)]
            [kListHandleFieldOffset] = handle;
        return true;
    }
    //INTR_Dict_Set: insert-or-replace (linear scan).
    if (intrinsicId == INTR_Dict_Set) {
        int32_t k, v;
        std::memcpy(&k, locals + callParamBase + kFrameSlotBytes, sizeof(k));
        std::memcpy(&v, locals + callParamBase + 2 * kFrameSlotBytes, sizeof(v));
        int32_t handle = ReadDictHandle(callParamBase, locals, "set");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { kv.second = v; return true; }
        }
        entries.push_back({k, v});
        return true;
    }
    //INTR_Dict_Get: lookup; throw on missing key.
    if (intrinsicId == INTR_Dict_Get) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + kFrameSlotBytes, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "get");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) {
                std::memcpy(pResult, &kv.second, sizeof(kv.second));
                return true;
            }
        }
        throw DeferredNlangRaise{m_exceptionClassIdx,
                                 "NLang VM: Dict key not found"};
    }
    //INTR_Dict_ContainsKey: 1 if found, 0 otherwise.
    if (intrinsicId == INTR_Dict_ContainsKey) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + kFrameSlotBytes, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "containsKey");
        auto& entries = m_dictStore[handle - 1].entries;
        int32_t result = 0;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { result = 1; break; }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    //INTR_Dict_Remove: 1 if removed, 0 if not found.
    if (intrinsicId == INTR_Dict_Remove) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + kFrameSlotBytes, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "remove");
        auto& entries = m_dictStore[handle - 1].entries;
        int32_t result = 0;
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (DictKeysEqual(it->first, k)) {
                entries.erase(it);
                result = 1;
                break;
            }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    //INTR_Dict_Clear: drop all entries.
    if (intrinsicId == INTR_Dict_Clear) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "clear");
        m_dictStore[handle - 1].entries.clear();
        return true;
    }
    //INTR_Dict_Count: entry count.
    if (intrinsicId == INTR_Dict_Count) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "count");
        int32_t n = static_cast<int32_t>(m_dictStore[handle - 1].entries.size());
        std::memcpy(pResult, &n, sizeof(n));
        return true;
    }
    //INTR_Dict_Keys (Phase 8e-5): allocate a fresh List<K> heap instance and
    //populate it with every dict entry's key (entries[i].first). The result
    //is a heap idx the caller treats as List<K>; element type K is known to
    //codegen (set when the resolver synthesized the List<K> return type), so
    //any subsequent Get(i) on the result will unbox correctly for primitive K.
    if (intrinsicId == INTR_Dict_Keys) {
        int32_t dictHandle = ReadDictHandle(callParamBase, locals, "keys");
        auto& src = m_dictStore[dictHandle - 1].entries;

        //Allocate the List<K> class instance on the heap + a fresh List handle.
        if (m_listClassIdx < 0)
            throw std::runtime_error("NLang VM: List class not registered");
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        int32_t listHandle  = AllocListHandle();
        m_structHeap[static_cast<size_t>(listHeapIdx)]
            [kListHandleFieldOffset] = listHandle;

        //Populate elements from dict keys.
        auto& dst = m_listStore[listHandle - 1].elements;
        dst.reserve(src.size());
        for (auto& kv : src)
            dst.push_back(kv.first);

        std::memcpy(pResult, &listHeapIdx, sizeof(listHeapIdx));
        NoteGcPressure();
        return true;
    }

    return false;
}

} // namespace nlang
