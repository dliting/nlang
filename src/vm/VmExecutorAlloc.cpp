/*---
    VmExecutorAlloc.cpp — 堆分配族（struct/class/array 记录物化与 struct 深拷贝）
    从 VmExecutorGC.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
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
    //0.7.5 Step A2: uniform 2-cell field slots — struct field i lives at
    //cell 2i, so the record is 2*fieldCount cells.
    const size_t recordCells = static_cast<size_t>(cs.fieldCount)
        * (kHeapFieldStrideBytes / kHeapCellBytes);
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(recordCells, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Struct;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = structIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.Emplace(recordCells, 0);
        m_slotKinds.Emplace(RTK_Struct);
        m_slotStructIdx.Emplace(structIdx);
    }
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(heapIdx)][i * 2] = innerIdx;
        }
    }
    //0.9.11 D4: per-field compile-time defaults (v2.6 metadata) apply at
    //every allocation — independent of constructors (which run only on
    //explicit construction). RTK_Void entries keep the zero fill; brace
    //initializers and constructor stores overwrite these afterwards.
    //Field i sits at cell 2i of the uniform 2-cell stride. The loop runs
    //after every nested allocation: MintNewString only grows the string
    //table, so no reference taken here can dangle across it.
    for (uint16_t i = 0; i < cs.fieldCount
         && i < cs.fieldDefaults.size(); ++i) {
        if (cs.fieldDefaults[i].tag == RTK_Void)
            continue;
        MaterializeDefaultValue(cs.fieldDefaults[i],
            reinterpret_cast<uint8_t*>(
                &m_structHeap[static_cast<size_t>(heapIdx)][i * 2]));
    }
    return heapIdx;
}

int32_t VmExecutor::AllocClassOnHeap(uint16_t classIdx) {
    if (classIdx >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in AllocClassOnHeap");
    //0.9.12 T1: field surface comes from the layout view (mirrors the
    //record today; joins the superclass chain from T2 on).
    const auto& fields = m_classLayout.Fields(classIdx);
    //0.7.5 Step A2: cell 0 = classIdx (1 cell), then 2 cells per data
    //field — field i lives at cell 1 + 2i.
    const size_t recordCells = 1 + fields.size()
        * (kHeapFieldStrideBytes / kHeapCellBytes);
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(recordCells, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Class;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.Emplace(recordCells, 0);
        m_slotKinds.Emplace(RTK_Class);
        m_slotStructIdx.Emplace(0);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] = static_cast<int32_t>(classIdx);
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].kind == RTK_Struct && fields[i].structIdx != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(fields[i].structIdx);
            m_structHeap[static_cast<size_t>(heapIdx)][i * 2 + 1] = innerIdx;
        }
    }
    return heapIdx;
}

int32_t VmExecutor::AllocArrayOnHeap(uint16_t arrayTypeIdx, int32_t size) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    //0.7.5: 8-byte scalar elements occupy two cells each (registry-
    //driven stride — see ArrayElemCells).
    const int cells = ArrayElemCells(at.elemKind);
    int32_t totalSlots = 3 + size * cells;
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(totalSlots, 0);
        m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Array;
        m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.Emplace(totalSlots, 0);
        m_slotKinds.Emplace(RTK_Array);
        m_slotStructIdx.Emplace(arrayTypeIdx);
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
    if (at.elemKind == RTK_Struct && at.elemTypeIdx != 0xFFFF) {
        for (int32_t i = 0; i < size; ++i) {
            int32_t elemIdx = AllocStructOnHeap(at.elemTypeIdx);
            m_structHeap[static_cast<size_t>(heapIdx)][3 + i * cells] = elemIdx;
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
        m_structHeap.Emplace(srcSlotCopy);
        m_slotKinds.Emplace(RTK_Struct);
        m_slotStructIdx.Emplace(structIdx);
    }
    //Deep-copy struct-typed fields (field i at cell 2i under the uniform
    //stride). Class-typed fields are shallow-copied (reference semantics
    //— the index value is copied as-is).
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (cs.fieldTypeKinds[i] == RTK_Struct
            && cs.fieldStructIndices[i] != 0xFFFF) {
            int32_t innerSrcIdx = srcSlotCopy[i * 2];
            int32_t innerNewIdx = DeepCopyStruct(innerSrcIdx,
                cs.fieldStructIndices[i]);
            m_structHeap[static_cast<size_t>(newHeapIdx)][i * 2] = innerNewIdx;
        }
    }
    //Task 2 gap fix: a pure deep-copy loop allocates records without ever
    //raising the pending flag — GC would never trigger on this path.
    NoteGcPressure();
    return newHeapIdx;
}

} // namespace nlang
