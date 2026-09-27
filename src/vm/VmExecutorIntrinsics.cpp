/*---
    VmExecutorIntrinsics.cpp — 内建函数分派（ExecuteIntrinsic）与 native 桥
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include "VmExecutorSer.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

void VmExecutor::RegisterNative(const std::string& name, NativeFn fn)
{
    m_natives[name] = fn;
}

void VmExecutor::CallNative(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    EnsureNativeAvailable(callee.name);
    auto it = m_natives.find(callee.name);
    if (it == m_natives.end())
        throw std::runtime_error(
            "NLang VM: native function not registered: " + callee.name);
    VmNativeHost host;
    InitNativeHost(host);
    it->second(&host.c, pResult, locals + callParamBase, callee.paramCount);
}

void VmExecutor::ExecuteIntrinsic(uint16_t intrinsicId, uint16_t callParamBase,
    uint8_t* locals, uint8_t* pResult)
{
    //ByteStream intrinsics (0-14): family delegation to
    //IntrinsicsByteStream.cpp (same shape as the math/io/string chain).
    if (ExecuteIntrinsicByteStream(intrinsicId, callParamBase, locals, pResult))
        return;

    //FileStream intrinsics (20-33): family delegation to
    //IntrinsicsFileStream.cpp.
    if (ExecuteIntrinsicFileStream(intrinsicId, callParamBase, locals, pResult))
        return;

    //Phase 8e-1: Object protocol intrinsics.
    //Object.Equals(this, other) → 0 or 1 (identity comparison on heap idx).
    //Object.GetHashCode(this) → heap idx as int32 (identity-based hash).
    //Both handle null: two nulls are equal, null has hash 0.
    if (intrinsicId == INTR_Object_Equals) {
        int32_t thisHeapIdx, otherHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        std::memcpy(&otherHeapIdx, locals + callParamBase + VALUE_SIZE,
                    sizeof(otherHeapIdx));
        int32_t result;
        if (thisHeapIdx == 0 && otherHeapIdx == 0)
            result = 1;  //two nulls are equal
        else if (thisHeapIdx == 0 || otherHeapIdx == 0)
            result = 0;  //one null, one non-null
        else
            result = (thisHeapIdx == otherHeapIdx) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    if (intrinsicId == INTR_Object_GetHashCode) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        int32_t result = (thisHeapIdx > 0) ? thisHeapIdx : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }

    //Phase 8e-1: String.Equals/GetHashCode migrated to
    //IntrinsicsString.cpp (ids unchanged); the Phase 11 Step 3 string
    //methods dispatch there too — see the family chain below.

    //Phase 8e-9b: Object.toString() default intrinsic.
    //Returns "TypeName@hex(heapIdx)" — Java-compat (lowercase, no padding).
    //Null receiver throws NPE. The hex uses heap idx directly (not user
    //override of getHashCode) — documented divergence from Java.
    if (intrinsicId == INTR_Object_toString) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            RaiseNlangException(m_nullPtrExcClassIdx,
                                "NLang VM: NullPointerException");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error("NLang VM: toString on invalid heap idx");
        //Boxed receiver: the payload IS the value — format it like the
        //matching primitive-to-string conversion (a boxed string's
        //toString shares the payload handle: string objects are
        //immutable, and handle 0 reads as "" everywhere).
        if (m_slotKinds[static_cast<size_t>(thisHeapIdx)] == RTK_Boxed) {
            int32_t tag = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
            int32_t val = m_structHeap[static_cast<size_t>(thisHeapIdx)][1];
            int32_t handle;
            if (tag == RTK_Int32) {
                handle = MintNewString(std::to_string(val));
            } else if (tag == RTK_Float) {
                float fv;
                std::memcpy(&fv, &val, sizeof(fv));
                char fbuf[32];
                std::snprintf(fbuf, sizeof(fbuf), "%g", fv);
                handle = MintNewString(fbuf);
            } else if (tag == RTK_String) {
                handle = val > 0 ? val : MintNewString(std::string());
            } else {
                throw std::runtime_error(
                    "NLang VM: toString on unsupported boxed type tag");
            }
            std::memcpy(pResult, &handle, sizeof(handle));
            return;
        }
        int32_t classIdx = m_structHeap[static_cast<size_t>(thisHeapIdx)][0];
        if (classIdx < 0
            || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
            throw std::runtime_error("NLang VM: toString on invalid class idx");
        const std::string& className =
            m_currModule->classes[static_cast<size_t>(classIdx)].name;
        char buf[64];
        //%x yields lowercase no-padding (Java Integer.toHexString-compatible)
        std::snprintf(buf, sizeof(buf), "%s@%x",
                      className.c_str(),
                      static_cast<unsigned>(thisHeapIdx));
        int32_t handle = MintNewString(buf);
        std::memcpy(pResult, &handle, sizeof(handle));
        return;
    }

    //Phase 9d: Exception ctor intrinsic. Shared by all 5 built-in
    //Exception subclasses (INTR_Exception_Ctor, INTR_NullPointer..,
    //INTR_DivByZero.., INTR_IndexOutOfBounds.., INTR_Assertion..).
    //Formals: (this, message). Writes message idx to slot[1], allocates
    //an empty List<string> and stores its heap idx in slot[2] (backtrace).
    //Direct writes (no held references) because AllocClassOnHeap may
    //reallocate m_structHeap mid-execution.
    if (intrinsicId == INTR_Exception_Ctor
        || intrinsicId == INTR_NullPointerException_Ctor
        || intrinsicId == INTR_DivByZeroException_Ctor
        || intrinsicId == INTR_IndexOutOfBoundsException_Ctor
        || intrinsicId == INTR_AssertionException_Ctor
        || intrinsicId == INTR_IOException_Ctor) {
        int32_t thisHeapIdx;
        std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
        if (thisHeapIdx <= 0)
            throw std::runtime_error(
                "NLang VM: Exception ctor on null instance");
        if (static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
            throw std::runtime_error(
                "NLang VM: Exception ctor on stale reference");

        //Read message formal (string handle).
        int32_t msgHandle;
        std::memcpy(&msgHandle, locals + callParamBase + VALUE_SIZE,
                    sizeof(msgHandle));

        //slot[1] = message (string handle). Direct write — safe because
        //no allocation between read and write.
        m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = msgHandle;

        //slot[2] = backtrace. Allocate a List<string>, set __handle = 0
        //(empty), then write its heap idx to slot[2]. The allocation may
        //reallocate m_structHeap, so we don't hold any references across
        //the AllocClassOnHeap call.
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        if (listHeapIdx > 0) {
            //__handle field (slot[1] of List instance) = 0 = empty
            m_structHeap[static_cast<size_t>(listHeapIdx)][1] = 0;
            m_structHeap[static_cast<size_t>(thisHeapIdx)][2] = listHeapIdx;
        }
        return;
    }

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
        return;
    }
    //INTR_List_Add: push element to list.
    if (intrinsicId == INTR_List_Add) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "add");
        m_listStore[handle - 1].elements.push_back(value);
        return;
    }
    //INTR_List_Get: return element at index.
    if (intrinsicId == INTR_List_Get) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "get");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            RaiseNlangException(m_oobExcClassIdx,
                                "NLang VM: List index out of bounds");
        int32_t val = lst.elements[idx];
        std::memcpy(pResult, &val, sizeof(val));
        return;
    }
    //INTR_List_Set: replace element at index.
    if (intrinsicId == INTR_List_Set) {
        int32_t idx, value;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        std::memcpy(&value, locals + callParamBase + 2 * VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "set");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            RaiseNlangException(m_oobExcClassIdx,
                                "NLang VM: List index out of bounds");
        lst.elements[idx] = value;
        return;
    }
    //INTR_List_Length: return elements.size().
    if (intrinsicId == INTR_List_Length) {
        int32_t handle = ReadListHandle(callParamBase, locals, "length");
        int32_t len = static_cast<int32_t>(m_listStore[handle - 1].elements.size());
        std::memcpy(pResult, &len, sizeof(len));
        return;
    }
    //INTR_List_RemoveAt: erase element at index.
    if (intrinsicId == INTR_List_RemoveAt) {
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + VALUE_SIZE, sizeof(idx));
        int32_t handle = ReadListHandle(callParamBase, locals, "removeAt");
        auto& lst = m_listStore[handle - 1];
        if (idx < 0 || static_cast<size_t>(idx) >= lst.elements.size())
            RaiseNlangException(m_oobExcClassIdx,
                                "NLang VM: List index out of bounds");
        lst.elements.erase(lst.elements.begin() + idx);
        return;
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
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "indexOf");
        int32_t result = FindListElement(m_listStore[handle - 1], value);
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_List_Contains: return 1 if found, 0 otherwise. Same primitive-vs-
    //class branching as IndexOf (C2 fix); matching lives in FindListElement.
    if (intrinsicId == INTR_List_Contains) {
        int32_t value;
        std::memcpy(&value, locals + callParamBase + VALUE_SIZE, sizeof(value));
        int32_t handle = ReadListHandle(callParamBase, locals, "contains");
        int32_t result =
            (FindListElement(m_listStore[handle - 1], value) >= 0) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_List_Clear: remove all elements.
    if (intrinsicId == INTR_List_Clear) {
        int32_t handle = ReadListHandle(callParamBase, locals, "clear");
        m_listStore[handle - 1].elements.clear();
        return;
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
        return;
    }
    //INTR_Dict_Set: insert-or-replace (linear scan).
    if (intrinsicId == INTR_Dict_Set) {
        int32_t k, v;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        std::memcpy(&v, locals + callParamBase + 2 * VALUE_SIZE, sizeof(v));
        int32_t handle = ReadDictHandle(callParamBase, locals, "set");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { kv.second = v; return; }
        }
        entries.push_back({k, v});
        return;
    }
    //INTR_Dict_Get: lookup; throw on missing key.
    if (intrinsicId == INTR_Dict_Get) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "get");
        auto& entries = m_dictStore[handle - 1].entries;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) {
                std::memcpy(pResult, &kv.second, sizeof(kv.second));
                return;
            }
        }
        RaiseNlangException(m_exceptionClassIdx, "NLang VM: Dict key not found");
    }
    //INTR_Dict_ContainsKey: 1 if found, 0 otherwise.
    if (intrinsicId == INTR_Dict_ContainsKey) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
        int32_t handle = ReadDictHandle(callParamBase, locals, "containsKey");
        auto& entries = m_dictStore[handle - 1].entries;
        int32_t result = 0;
        for (auto& kv : entries) {
            if (DictKeysEqual(kv.first, k)) { result = 1; break; }
        }
        std::memcpy(pResult, &result, sizeof(result));
        return;
    }
    //INTR_Dict_Remove: 1 if removed, 0 if not found.
    if (intrinsicId == INTR_Dict_Remove) {
        int32_t k;
        std::memcpy(&k, locals + callParamBase + VALUE_SIZE, sizeof(k));
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
        return;
    }
    //INTR_Dict_Clear: drop all entries.
    if (intrinsicId == INTR_Dict_Clear) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "clear");
        m_dictStore[handle - 1].entries.clear();
        return;
    }
    //INTR_Dict_Count: entry count.
    if (intrinsicId == INTR_Dict_Count) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "count");
        int32_t n = static_cast<int32_t>(m_dictStore[handle - 1].entries.size());
        std::memcpy(pResult, &n, sizeof(n));
        return;
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
        m_gcPending = true;
        return;
    }

    //INTR_List_toString (Phase 9b-pre): format the list's elements as
    //"[e1, e2, ...]" via FormatList. Mint a string object, write its
    //handle to pResult. Strings inside are quoted via QuoteString.
    if (intrinsicId == INTR_List_toString) {
        int32_t handle = ReadListHandle(callParamBase, locals, "toString");
        std::string s = FormatList(handle, 0);
        int32_t strHandle = MintNewString(std::move(s));
        std::memcpy(pResult, &strHandle, sizeof(strHandle));
        return;
    }
    //INTR_Dict_toString (Phase 9b-pre): format the dict's entries as
    //"{k1: v1, k2: v2, ...}" via FormatDict.
    if (intrinsicId == INTR_Dict_toString) {
        int32_t handle = ReadDictHandle(callParamBase, locals, "toString");
        std::string s = FormatDict(handle, 0);
        int32_t strHandle = MintNewString(std::move(s));
        std::memcpy(pResult, &strHandle, sizeof(strHandle));
        return;
    }

    //Phase 11: stdlib namespace functions (free-function ABI — args from
    //callParamBase slot 0, no this). Chained before the unknown-id throw
    //so each family TU stays independently extensible.
    if (ExecuteIntrinsicMath(intrinsicId, callParamBase, locals, pResult))
        return;
    if (ExecuteIntrinsicIo(intrinsicId, callParamBase, locals, pResult))
        return;
    if (ExecuteIntrinsicString(intrinsicId, callParamBase, locals, pResult))
        return;
    if (ExecuteIntrinsicFs(intrinsicId, callParamBase, locals, pResult))
        return;

    throw std::runtime_error("NLang VM: unknown intrinsic id " + std::to_string(intrinsicId));
}

} // namespace nlang
