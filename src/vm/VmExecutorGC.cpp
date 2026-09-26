/*---
    VmExecutorGC.cpp — 垃圾回收（标记/清扫/释放）与堆分配
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

int32_t VmExecutor::AllocStructOnHeap(uint16_t structIdx) {
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in AllocStruct");
    const auto& cs = m_currModule->structs[structIdx];
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(cs.fieldCount, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Struct;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = structIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(cs.fieldCount, 0);
        m_slotKinds.push_back(RTK_Struct);
        m_slotStructIdx.push_back(structIdx);
    }
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(heapIdx)][i] = innerIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::AllocClassOnHeap(uint16_t classIdx) {
    if (classIdx >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in AllocClassOnHeap");
    const auto& cc = m_currModule->classes[classIdx];
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(cc.fieldCount + 1, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Class;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(cc.fieldCount + 1, 0);
        m_slotKinds.push_back(RTK_Class);
        m_slotStructIdx.push_back(0);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = static_cast<int32_t>(classIdx);
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (cc.fieldTypeKinds[i] == RTK_Struct
            && cc.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(cc.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(heapIdx)][i + 1] = innerIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::AllocArrayOnHeap(uint16_t arrayTypeIdx, int32_t size) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    int32_t totalSlots = 3 + size;
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(totalSlots, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Array;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(totalSlots, 0);
        m_slotKinds.push_back(RTK_Array);
        m_slotStructIdx.push_back(arrayTypeIdx);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = RTK_Array;
    m_structHeap[static_cast<size_t>(heapIdx)][1] = arrayTypeIdx;
    m_structHeap[static_cast<size_t>(heapIdx)][2] = size;
    //Phase 9d-3: struct-typed elements have value semantics — materialize
    //a fresh struct per element, mirroring AllocStructOnHeap's recursive
    //materialization of nested struct fields. MarkPhase already traces
    //RTK_Struct array elements, so the materialized structs stay reachable.
    //Write elements by index (AllocStructOnHeap may grow m_structHeap and
    //reallocate the outer vector; no reference is held across the call).
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    if (at.elemKind == RTK_Struct && at.elemTypeIdx != 0xFFFF) {
        for (int32_t i = 0; i < size; ++i) {
            int32_t elemIdx = AllocStructOnHeap(at.elemTypeIdx);
            m_structHeap[static_cast<size_t>(heapIdx)][3 + i] = elemIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::DeepCopyStruct(int32_t srcHeapIdx, uint16_t structIdx) {
    if (srcHeapIdx < 0 || static_cast<size_t>(srcHeapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in CopyStruct");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in CopyStruct");
    const auto& cs = m_currModule->structs[structIdx];
    auto srcSlotCopy = m_structHeap[static_cast<size_t>(srcHeapIdx)];
    int32_t newHeapIdx;
    if (!m_freeList.empty()) {
        newHeapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(newHeapIdx)] = srcSlotCopy;
        m_slotKinds[static_cast<size_t>(newHeapIdx)] = RTK_Struct;
        m_slotStructIdx[static_cast<size_t>(newHeapIdx)] = structIdx;
    } else {
        newHeapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.push_back(srcSlotCopy);
        m_slotKinds.push_back(RTK_Struct);
        m_slotStructIdx.push_back(structIdx);
    }
    //Deep-copy struct-typed fields. Class-typed fields are shallow-copied
    //(reference semantics — the index value is copied as-is).
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerSrcIdx = srcSlotCopy[i];
            int32_t innerNewIdx = DeepCopyStruct(innerSrcIdx,
                cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(newHeapIdx)][i] = innerNewIdx;
        }
    }
    //Task 2 gap fix: a pure deep-copy loop allocates records without ever
    //raising the pending flag — GC would never trigger on this path.
    m_gcPending = true;
    return newHeapIdx;
}

//GC implementation.

void VmExecutor::CheckGCSafepoint() {
    //String objectization: pure-string workloads allocate zero heap
    //records, so the string store has its own threshold arm.
    if (m_gcPending && (m_structHeap.size() > m_gcThreshold
            || m_stringObjs.size() > m_strGcThreshold)) {
        m_gcPending = false;
        CollectGarbage();
    }
}

void VmExecutor::CollectGarbage() {
    MarkPhase();
    SweepPhase();
    //String sweep runs after the heap sweep, with marking completed for
    //both stores (one mark phase fills both bit vectors).
    SweepStrings();
}

namespace {
//Single source for the container trace arms' kind set (was four
//hand-expanded literals: List/Dict x mark/push). At kind granularity the
//mark set and the worklist push set coincide — every traced kind has
//children for the dispatch below to walk (Func's receiver, array
//elements, boxed payload, class/struct fields).
inline bool IsChildBearingHeapKind(uint8_t kind) {
    return kind == RTK_Class || kind == RTK_Struct || kind == RTK_Boxed
        || kind == RTK_Func || kind == RTK_Array;
}
} // namespace

void VmExecutor::MarkPhase() {
    m_markBits.assign(m_structHeap.size(), false);
    m_strMarkBits.assign(m_stringObjs.size(), false);
    //Identify root references from all call frames and push to worklist.
    std::vector<int32_t> worklist;
    for (auto& frame : m_callStack) {
        for (auto& ld : frame.func->locals) {
            if (ld.typeKind != RTK_Struct && ld.typeKind != RTK_Class
                && ld.typeKind != RTK_Array && ld.typeKind != RTK_Func
                && ld.typeKind != RTK_String)
                continue;
            int32_t val;
            std::memcpy(&val, frame.locals + ld.offset, sizeof(val));
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
            if (!m_markBits[val]) {
                m_markBits[val] = true;
                worklist.push_back(val);
            }
        }
        if (frame.pResult) {
            uint8_t retKind = frame.func->returnTypeKind;
            int32_t val;
            std::memcpy(&val, frame.pResult, sizeof(val));
            //String return slot: same handle-not-heap-index split as locals.
            //Deliberately conservative (adjudicated over-mark, spec D6
            //note): a pResult-resident string is also reachable from the
            //callee's own roots, so no source shape can pin this face red.
            //Kept as defense in depth — do not "tighten" it into an
            //under-mark.
            if (retKind == RTK_String) {
                MarkString(val);
            } else if (retKind == RTK_Class || retKind == RTK_Struct
                || retKind == RTK_Array || retKind == RTK_Func) {
                if (val > 0 && static_cast<size_t>(val) < m_slotKinds.size()
                    && !m_markBits[val]) {
                    m_markBits[val] = true;
                    worklist.push_back(val);
                }
            }
        }
    }
    //Iteratively trace references until worklist is empty.
    while (!worklist.empty()) {
        int32_t idx = worklist.back();
        worklist.pop_back();
        if (m_slotKinds[idx] == RTK_Func) {
            //Phase 13: trace the captured receiver (slot[1]) of a bound
            //handle; slot[0] is a function/name index, not a heap slot.
            int32_t thisIdx = m_structHeap[idx][1];
            if (thisIdx > 0
                && static_cast<size_t>(thisIdx) < m_slotKinds.size()
                && m_slotKinds[static_cast<size_t>(thisIdx)] == RTK_Class
                && !m_markBits[static_cast<size_t>(thisIdx)]) {
                m_markBits[static_cast<size_t>(thisIdx)] = true;
                worklist.push_back(thisIdx);
            }
        } else if (m_slotKinds[idx] == RTK_Boxed) {
            //String objectization: a boxed string's payload (slot[1]) is a
            //string handle. Other boxed tags carry raw bits (no children).
            if (m_structHeap[idx][0] == RTK_String)
                MarkString(m_structHeap[idx][1]);
        } else if (m_slotKinds[idx] == RTK_Class) {
            int32_t classIdx = m_structHeap[idx][0];
            auto& cc = m_currModule->classes[classIdx];
            for (uint16_t i = 0; i < cc.fieldCount; ++i) {
                int32_t refIdx = m_structHeap[idx][i + 1];
                //String fields carry a string-store handle, not a heap
                //index — route before the heap-index guard (a valid handle
                //may lie beyond the struct heap's size). No runtime slot-
                //kind double condition: the string store has no slotKinds.
                if (cc.fieldTypeKinds[i] == RTK_String) {
                    MarkString(refIdx);
                    continue;
                }
                if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
                    continue;
                //Array redesign B: field kinds come from the declared type,
                //so RTK_Array fields are routed explicitly (same declared-
                //plus-runtime double condition as the reference kinds).
                //The old unconditional runtime-kind fallback is safe to drop:
                //jagged declaration forms that could smuggle an array record
                //into a non-array field slot are rejected at resolve time.
                //Pre-fix v1.9 modules also misfile array fields as
                //RTK_Int32 here; the v1.10 loader floor (same release)
                //refuses them, closing the GC under-trace window.
                //Object-declared fields (declared kind RTK_Class) also
                //hold boxed primitive records (runtime kind RTK_Boxed) —
                //push those too; the worklist's boxed arm marks a wrapped
                //string payload. Without this arm the sweep frees the
                //box while the field still points at it.
                if ((cc.fieldTypeKinds[i] == RTK_Class
                        && (m_slotKinds[refIdx] == RTK_Class
                            || m_slotKinds[refIdx] == RTK_Boxed))
                    || (cc.fieldTypeKinds[i] == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)
                    || (cc.fieldTypeKinds[i] == RTK_Func && m_slotKinds[refIdx] == RTK_Func)
                    || (cc.fieldTypeKinds[i] == RTK_Array && m_slotKinds[refIdx] == RTK_Array)) {
                    if (!m_markBits[refIdx]) {
                        m_markBits[refIdx] = true;
                        worklist.push_back(refIdx);
                    }
                }
            }
            //Phase 8e-3: trace List<T> elements as additional GC roots.
            if (static_cast<int16_t>(classIdx) == m_listClassIdx) {
                int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
                if (handle > 0) {
                    size_t h = static_cast<size_t>(handle - 1);
                    if (h < m_listStore.size()) {
                        for (int32_t elem : m_listStore[h].elements) {
                            if (elem > 0
                                && static_cast<size_t>(elem) < m_slotKinds.size()
                                && !m_markBits[elem]) {
                                if (IsChildBearingHeapKind(m_slotKinds[elem])) {
                                    m_markBits[elem] = true;
                                    worklist.push_back(elem);
                                }
                            }
                        }
                    }
                }
            }
            //Phase 8e-4: trace Dict<K,V> entries (both K and V are heap idxs).
            if (static_cast<int16_t>(classIdx) == m_dictClassIdx) {
                int32_t handle = m_structHeap[idx][kListHandleFieldOffset];
                if (handle > 0) {
                    size_t h = static_cast<size_t>(handle - 1);
                    if (h < m_dictStore.size()) {
                        for (auto& kv : m_dictStore[h].entries) {
                            for (int32_t elem : {kv.first, kv.second}) {
                                if (elem > 0
                                    && static_cast<size_t>(elem) < m_slotKinds.size()
                                    && !m_markBits[elem]) {
                                    if (IsChildBearingHeapKind(m_slotKinds[elem])) {
                                        m_markBits[elem] = true;
                                        worklist.push_back(elem);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (m_slotKinds[idx] == RTK_Struct) {
            uint16_t structIdx = m_slotStructIdx[idx];
            auto& cs = m_currModule->structs[structIdx];
            for (uint16_t i = 0; i < cs.fieldCount; ++i) {
                int32_t refIdx = m_structHeap[idx][i];
                //String fields carry a string-store handle, not a heap
                //index — same pre-guard routing as the class field arm.
                if (cs.fieldTypeKinds[i] == RTK_String) {
                    MarkString(refIdx);
                    continue;
                }
                if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
                    continue;
                //Explicit RTK_Array route (declared kind now authoritative) —
                //see the RTK_Class branch above for why the old runtime-kind
                //fallback is gone, including the v1.10 module-floor coupling.
                //Same Object-field boxed-record acceptance as the class
                //field arm above (an Object field inside a struct).
                if ((cs.fieldTypeKinds[i] == RTK_Class
                        && (m_slotKinds[refIdx] == RTK_Class
                            || m_slotKinds[refIdx] == RTK_Boxed))
                    || (cs.fieldTypeKinds[i] == RTK_Struct && m_slotKinds[refIdx] == RTK_Struct)
                    || (cs.fieldTypeKinds[i] == RTK_Func && m_slotKinds[refIdx] == RTK_Func)
                    || (cs.fieldTypeKinds[i] == RTK_Array && m_slotKinds[refIdx] == RTK_Array)) {
                    if (!m_markBits[refIdx]) {
                        m_markBits[refIdx] = true;
                        worklist.push_back(refIdx);
                    }
                }
            }
        } else if (m_slotKinds[idx] == RTK_Array) {
            uint16_t arrayTypeIdx = m_slotStructIdx[idx];
            auto& at = m_currModule->arrayTypes[arrayTypeIdx];
            int32_t length = m_structHeap[idx][2];
            for (int32_t i = 0; i < length; ++i) {
                int32_t elemRef = m_structHeap[idx][3 + i];
                //String elements are raw string handles (hole-1 uses 0 =
                //null) — same pre-guard routing as the field arms.
                if (at.elemKind == RTK_String) {
                    MarkString(elemRef);
                    continue;
                }
                if (elemRef <= 0 || static_cast<size_t>(elemRef) >= m_slotKinds.size())
                    continue;
                //Object[] elements carry boxed primitive records (elemKind
                //RTK_Class, runtime kind RTK_Boxed) — same acceptance as
                //the field arms.
                if (at.elemKind == RTK_Class
                        && (m_slotKinds[elemRef] == RTK_Class
                            || m_slotKinds[elemRef] == RTK_Boxed)
                    && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
                else if (at.elemKind == RTK_Struct && m_slotKinds[elemRef] == RTK_Struct
                         && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
                else if (at.elemKind == RTK_Func
                         && m_slotKinds[elemRef] == RTK_Func
                         && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
                //Array redesign B (spec §5.3#1): an array whose ELEMENTS are
                //themselves array records (elemKind == RTK_Array). Currently
                //unreachable from compilable source (jagged declarations are
                //rejected; List<int[]> elements live in the container store) —
                //defensive base for future/external .nmod paths. Declared
                //elemKind and runtime slot kind must BOTH be RTK_Array (same
                //double condition as the field arms above).
                else if (at.elemKind == RTK_Array
                         && m_slotKinds[elemRef] == RTK_Array
                         && !m_markBits[elemRef]) {
                    m_markBits[elemRef] = true;
                    worklist.push_back(elemRef);
                }
            }
        }
    }
}

void VmExecutor::SweepPhase() {
    m_freeList.clear();
    for (size_t i = 1; i < m_structHeap.size(); ++i) {
        if (m_slotKinds[i] == 0) continue;
        if (!m_markBits[i]) {
            if (m_slotKinds[i] == RTK_Class) {
                //Phase 8e-3: recycle List<T> handle when a List instance is freed.
                int32_t classIdx = m_structHeap[i][0];
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
                FreeOwnedStructs(static_cast<int32_t>(i));
            }
            if (m_slotKinds[i] == RTK_Struct)
                FreeNestedStructs(static_cast<int32_t>(i), m_slotStructIdx[i]);
            if (m_slotKinds[i] == RTK_Array)
                FreeOwnedArrayStructElements(static_cast<int32_t>(i));
            m_structHeap[i].clear();
            m_slotKinds[i] = 0;
            m_freeList.push_back(static_cast<int32_t>(i));
        }
    }
}

void VmExecutor::FreeOwnedStructs(int32_t heapIdx) {
    int32_t classIdx = m_structHeap[heapIdx][0];
    auto& cc = m_currModule->classes[classIdx];
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (cc.fieldTypeKinds[i] == RTK_Struct
            && cc.fieldStructIndices[i] != 0xFFFF) {
            int32_t structSlotIdx = m_structHeap[heapIdx][i + 1];
            if (structSlotIdx > 0 && static_cast<size_t>(structSlotIdx) < m_slotKinds.size()
                && m_slotKinds[structSlotIdx] == RTK_Struct) {
                FreeNestedStructs(structSlotIdx, cc.fieldStructIndices[i]);
                m_structHeap[structSlotIdx].clear();
                m_slotKinds[structSlotIdx] = 0;
                m_freeList.push_back(structSlotIdx);
            }
        }
    }
}

void VmExecutor::FreeNestedStructs(int32_t heapIdx, uint16_t structIdx) {
    auto& cs = m_currModule->structs[structIdx];
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t nestedIdx = m_structHeap[heapIdx][i];
            if (nestedIdx > 0 && static_cast<size_t>(nestedIdx) < m_slotKinds.size()
                && m_slotKinds[nestedIdx] == RTK_Struct) {
                FreeNestedStructs(nestedIdx, cs.fieldStructIndices[i]);
                m_structHeap[nestedIdx].clear();
                m_slotKinds[nestedIdx] = 0;
                m_freeList.push_back(nestedIdx);
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
            m_freeList.push_back(elemRef);
        }
    }
}

} // namespace nlang
