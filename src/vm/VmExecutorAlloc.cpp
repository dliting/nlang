/*---
    VmExecutorAlloc.cpp — 堆分配族（struct/class/array 记录物化与 struct 深拷贝）
    从 VmExecutorGC.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

//K8 sharding: the calling thread's shard (carrier id, or the host
//fallback slot past the carrier range for embed threads).
size_t VmExecutor::CurrentShard() const {
    const int id = m_spScheduler->CurrentCarrierId();
    //host fallback threads (embed bridge) use the shard past the range
    return id >= 0 ? static_cast<size_t>(id)
                   : static_cast<size_t>(m_carrierThreadCount);
}

int32_t VmExecutor::TakeHeapSlot() {
    const size_t shard = CurrentShard();
    if (m_heapShards.Empty(shard)) {
        DomainGuard g(m_domainLocks, DomainId::Growth);
        if (m_heapShards.Empty(shard))   //double-check under the lock
            GrowHeapShardBlockLocked(shard);
    }
    return m_heapShards.Pop(shard);
}

void VmExecutor::GrowHeapShardBlockLocked(size_t shard) {
    const int32_t base = static_cast<int32_t>(m_structHeap.size());
    for (size_t k = 0; k < m_allocQuantum; ++k) {
        m_structHeap.Emplace();
        m_slotKinds.Emplace(0);
        m_slotStructIdx.Emplace(0);
    }
    m_heapShards.PushBlockDescending(shard, base, m_allocQuantum);
}

void VmExecutor::HeapSweepPush(int32_t idx) {
    m_heapShards.Push(m_heapShards.SweepShardOf(idx), idx);
}

int32_t VmExecutor::AllocStructOnHeap(uint16_t structIdx) {
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in AllocStruct");
    const auto& cs = m_currModule->structs[structIdx];
    //0.7.5 Step A2: uniform 2-cell field slots — struct field i lives at
    //cell 2i, so the record is 2*fieldCount cells.
    const size_t recordCells = static_cast<size_t>(cs.fieldCount)
        * (kHeapFieldStrideBytes / kHeapCellBytes);
    const int32_t heapIdx = TakeHeapSlot();
    m_structHeap[static_cast<size_t>(heapIdx)].assign(recordCells, 0);
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Struct;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = structIdx;
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
    const int32_t heapIdx = TakeHeapSlot();
    m_structHeap[static_cast<size_t>(heapIdx)].assign(recordCells, 0);
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Class;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    m_structHeap[static_cast<size_t>(heapIdx)][0] = static_cast<int32_t>(classIdx);
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].kind == RTK_Struct && fields[i].structIdx != 0xFFFF) {
            int32_t innerIdx = AllocStructOnHeap(fields[i].structIdx);
            m_structHeap[static_cast<size_t>(heapIdx)][i * 2 + 1] = innerIdx;
        }
    }
    return heapIdx;
}

//The single length gate (language creation and native minting): every
//dimension must be non-negative and within kMaxArrayElems, and the
//product (elemCount) must stay within it too — int64 accumulation so
//the product itself cannot overflow the check.
int32_t VmExecutor::ValidateArrayDims(uint8_t rank, const int32_t* dims)
{
    int64_t elemCount = 1;
    for (uint8_t d = 0; d < rank; ++d)
    {
        if (dims[d] < 0 || dims[d] > kMaxArrayElems)
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array length out of range");
        elemCount *= dims[d];
        if (elemCount > kMaxArrayElems)
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array length out of range");
    }
    return static_cast<int32_t>(elemCount);
}

int32_t VmExecutor::AllocArrayOnHeap(uint16_t arrayTypeIdx, uint8_t rank,
    const int32_t* dims) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    const int32_t size = ValidateArrayDims(rank, dims);
    //0.7.5: 8-byte scalar elements occupy two cells each (registry-
    //driven stride — see ArrayElemCells).
    const int cells = ArrayElemCells(at.elemKind);
    int32_t totalSlots =
        static_cast<int32_t>(ArrayHeaderCells(rank)) + size * cells;
    //The one array allocation path shared by language creation and
    //native minting: raise the collector-pending flag here, so a loop
    //that only allocates through natives still reaches collection.
    NoteGcPressure();
    const int32_t heapIdx = TakeHeapSlot();
    m_structHeap[static_cast<size_t>(heapIdx)].assign(totalSlots, 0);
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Array;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    auto& record = m_structHeap[static_cast<size_t>(heapIdx)];
    record[0] = RTK_Array;
    record[1] = arrayTypeIdx;
    record[2] = rank;
    record[3] = size;
    for (uint8_t d = 0; d < rank; ++d) record[4 + d] = dims[d];
    //Phase 9d-3: struct-typed elements have value semantics — a fresh
    //struct per element (MarkPhase traces RTK_Struct array elements).
    //Write by index: AllocStructOnHeap may reallocate the outer vector.
    if (at.elemKind == RTK_Struct && at.elemTypeIdx != 0xFFFF) {
        const int32_t base =
            static_cast<int32_t>(ArrayHeaderCells(rank));
        for (int32_t i = 0; i < size; ++i) {
            int32_t elemIdx = AllocStructOnHeap(at.elemTypeIdx);
            m_structHeap[static_cast<size_t>(heapIdx)]
                [base + i * cells] = elemIdx;
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
    const int32_t newHeapIdx = TakeHeapSlot();
    m_structHeap[static_cast<size_t>(newHeapIdx)] = srcSlotCopy;
    m_slotKinds[static_cast<size_t>(newHeapIdx)] = RTK_Struct;
    m_slotStructIdx[static_cast<size_t>(newHeapIdx)] = structIdx;
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
