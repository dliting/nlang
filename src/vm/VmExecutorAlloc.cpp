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
        m_instDescByHeap.Emplace(nullptr);
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
    //D5 writer side: free-list slots recycle across kinds, so this slot
    //must not inherit a previous tenant's descriptor stamp.
    m_instDescByHeap[static_cast<size_t>(heapIdx)] = nullptr;
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

//v0.13.1: intern the module's static descriptor rows so OP_New's
//descIdx operand resolves to a pointer. Static rows rebuild their
//argument trees from the canonical spelling (leaf link keys resolve
//against the linked module's tables); recipe rows (runtime composition)
//stay null until that path lands.
void VmExecutor::InitDescriptorTable(const CompiledModule& module) {
    m_descCache.clear();
    m_descCache.reserve(module.descriptorTable.size());
    for (const auto& entry : module.descriptorTable) {
        m_descCache.push_back(entry.recipe.empty()
            ? InternDescriptorFromSpelling(entry.spelling, module)
            : nullptr);
    }
}

int32_t VmExecutor::AllocClassOnHeap(uint16_t classIdx,
                                     GenericInstDescriptor* instDesc) {
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
    m_instDescByHeap[static_cast<size_t>(heapIdx)] = instDesc;
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
    //v0.13.1 D5: clear the descriptor carrier cell — free-list slots
    //recycle across kinds, and the element stamp write (StampElemDescriptor)
    //is optional, so a previous tenant's stamp must not survive here.
    m_instDescByHeap[static_cast<size_t>(heapIdx)] = nullptr;
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

//Validation half of AllocViewOnHeap (split for the function-size guard):
//target-handle families, the depth-one peel, type identity, dims/
//product, and coverage. Returns the peeled OWNED target handle and
//carries the validated elemCount out. dims/strides/offset are OWNER-
//payload-space numbers — every real caller derives them from a record
//header (view headers already store owner-space strides), so the fold
//never re-derives them; a view target's cell is already the owned
//record (maintained by induction), the fold just rewrites the handle.
int32_t VmExecutor::ValidateViewCreation(uint16_t arrayTypeIdx,
    uint8_t rank, const int32_t* dims, const int32_t* strides,
    int32_t offset, int32_t targetIdx, int32_t& elemCountOut) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    if (rank < 1 || rank > kMaxArrayRank)
        RaiseNlangException(m_oobExcClassIdx, "view rank out of range");
    if (targetIdx < 0
        || static_cast<size_t>(targetIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: invalid heap index in AllocView");
    if (targetIdx == 0)
        RaiseNlangException(m_nullPtrExcClassIdx, "view of a null array");
    //Depth-one fold: peel a view target to its owned record; the peeled
    //handle re-validates (a corrupt tail must not read OOB below).
    if (m_slotKinds[static_cast<size_t>(targetIdx)] == RTK_ArrayView) {
        const int32_t viewRank =
            m_structHeap[static_cast<size_t>(targetIdx)][2];
        targetIdx = m_structHeap[static_cast<size_t>(targetIdx)]
            [ArrayViewTargetCell(static_cast<uint8_t>(viewRank))];
        if (targetIdx <= 0
            || static_cast<size_t>(targetIdx) >= m_structHeap.size())
            throw std::runtime_error(
                "NLang VM: corrupt view record (target out of range)");
    }
    if (m_slotKinds[static_cast<size_t>(targetIdx)] != RTK_Array)
        RaiseNlangException(m_oobExcClassIdx,
            "view target is not an array");
    //Per-dim and product validation (negative dims, the 100M cap) — the
    //same gate owned creation uses; raises the OutOfBounds family.
    const int32_t size = ValidateArrayDims(rank, dims);
    ValidateViewTypeMatch(arrayTypeIdx, rank, targetIdx);
    //Coverage: the smallest and largest addresses the affine map can
    //produce must land inside the owner's element range (zero strides
    //and negative axes included — they fold into min/max naturally).
    //int64 terms: dims are within the cap and every dim of a non-empty
    //product is >= 1, so at most log2(kMaxArrayElems) terms are nonzero
    //and each (dims[d]-1)*strides[d] fits — the sum cannot overflow.
    if (size > 0)
        ValidateViewCoverage(rank, dims, strides, offset,
            static_cast<int64_t>(
                m_structHeap[static_cast<size_t>(targetIdx)][3]));
    elemCountOut = size;
    return targetIdx;
}

//Type identity at the ELEMENT level (function-size split of the view
//validation): the caller's row must name this view's own rank (record
//cells [1] and [2] stay consistent — rank-reduced views like m[1, :]
//carry their own row, not the owner's) and the same element as the
//owner's row. Rank itself may differ — slicing changes it. The heap
//cell reads happen as rvalues (no reference held across the raise,
//which may grow the store); the arrayTypes rows live on the immutable
//module and stay stable.
void VmExecutor::ValidateViewTypeMatch(uint16_t arrayTypeIdx,
                                       uint8_t rank, int32_t ownerIdx) {
    const auto& callerRow = m_currModule->arrayTypes[arrayTypeIdx];
    const auto& ownerRow =
        m_currModule->arrayTypes[static_cast<size_t>(
            static_cast<uint16_t>(
                m_structHeap[static_cast<size_t>(ownerIdx)][1]))];
    if (callerRow.rank != rank
        || callerRow.elemKind != ownerRow.elemKind
        || callerRow.elemTypeIdx != ownerRow.elemTypeIdx)
        RaiseNlangException(m_oobExcClassIdx,
            "view element type does not match the target array");
}

//Coverage half of the view validation (function-size split): the
//smallest and largest addresses the affine map can produce must land
//inside the owner's element range (zero strides and negative axes
//included — they fold into min/max naturally). int64 terms: dims are
//within the cap and every dim of a non-empty product is >= 1, so at
//most log2(kMaxArrayElems) terms are nonzero and each
//(dims[d]-1)*strides[d] fits — the sum cannot overflow.
void VmExecutor::ValidateViewCoverage(uint8_t rank, const int32_t* dims,
    const int32_t* strides, int32_t offset, int64_t ownerCount) {
    int64_t minAddr = offset;
    int64_t maxAddr = offset;
    for (uint8_t d = 0; d < rank; ++d) {
        const int64_t span = static_cast<int64_t>(dims[d] - 1)
            * static_cast<int64_t>(strides[d]);
        if (span < 0) minAddr += span; else maxAddr += span;
    }
    if (minAddr < 0 || maxAddr >= ownerCount)
        RaiseNlangException(m_oobExcClassIdx,
            "view range exceeds the target array bounds");
}

//The single view-creation choke point (v0.12.7): validate, then
//materialize an RTK_ArrayView record. All target reads happen before
//the slot take — TakeHeapSlot may grow the store and invalidate held
//references (the AllocArrayOnHeap contract).
int32_t VmExecutor::AllocViewOnHeap(uint16_t arrayTypeIdx, uint8_t rank,
    const int32_t* dims, const int32_t* strides, int32_t offset,
    int32_t targetIdx) {
    int32_t elemCount = 0;
    targetIdx = ValidateViewCreation(arrayTypeIdx, rank, dims, strides,
                                     offset, targetIdx, elemCount);
    NoteGcPressure();
    const int32_t heapIdx = TakeHeapSlot();
    m_structHeap[static_cast<size_t>(heapIdx)].assign(
        ArrayViewRecordCells(rank), 0);
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_ArrayView;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    //D5 writer side: free-list slots recycle across kinds, so this slot
    //must not inherit a previous tenant's descriptor stamp.
    m_instDescByHeap[static_cast<size_t>(heapIdx)] = nullptr;
    auto& record = m_structHeap[static_cast<size_t>(heapIdx)];
    record[0] = RTK_ArrayView;
    record[1] = arrayTypeIdx;
    record[2] = rank;
    record[3] = elemCount;
    for (uint8_t d = 0; d < rank; ++d) {
        record[4 + d] = dims[d];
        record[ArrayViewStridesBase(rank) + d] = strides[d];
    }
    record[ArrayViewOffsetCell(rank)] = offset;
    record[ArrayViewTargetCell(rank)] = targetIdx;
    return heapIdx;
}

//v0.12.7 Task 13: fresh owning record of the view's own dims, each
//element read through the view's affine map (the serializer's
//materialization — the view itself is untouched). The copy loop
//allocates nothing, so references taken after AllocArrayOnHeap stay
//valid; struct elements alias the source structs (value-copying them
//here would be dead work — the transient copy is written to a stream
//and dropped).
int32_t VmExecutor::MaterializeViewRecord(int32_t viewHeapIdx) {
    if (viewHeapIdx <= 0
        || static_cast<size_t>(viewHeapIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(viewHeapIdx)] != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: invalid view record in MaterializeView");
    const uint8_t rank =
        static_cast<uint8_t>(m_structHeap[static_cast<size_t>(viewHeapIdx)][2]);
    const uint16_t arrayTypeIdx = static_cast<uint16_t>(
        m_structHeap[static_cast<size_t>(viewHeapIdx)][1]);
    int32_t dims[kMaxArrayRank] = {0};
    for (uint8_t d = 0; d < rank; ++d)
        dims[d] = m_structHeap[static_cast<size_t>(viewHeapIdx)][4 + d];
    const int32_t elemCount =
        m_structHeap[static_cast<size_t>(viewHeapIdx)][3];

    const int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx, rank, dims);
    if (elemCount == 0)
        return heapIdx;

    const uint8_t elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    const int cells = ArrayElemCells(elemKind);
    size_t ownerBase = 0;
    auto& owner = ElementPayloadRecord(
        m_structHeap[static_cast<size_t>(viewHeapIdx)], RTK_ArrayView,
        ownerBase);
    const auto& view = m_structHeap[static_cast<size_t>(viewHeapIdx)];
    auto& dst = m_structHeap[static_cast<size_t>(heapIdx)];
    const size_t dstBase = ArrayHeaderCells(rank);
    for (int32_t flat = 0; flat < elemCount; ++flat) {
        const int64_t addr = ViewFlatElementAddress(view, flat);
        for (int c = 0; c < cells; ++c)
            dst[dstBase + static_cast<size_t>(flat) * cells + c] =
                owner[ownerBase + static_cast<size_t>(addr) * cells + c];
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
