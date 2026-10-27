/*---
    VmExecutorOpsArrayN.cpp - ND array-record opcodes: allocation, per-dim
    and flat element access, shape export (stage-2) and row-handle
    assembly (stage-3 dynamic initializers). Moved verbatim from
    VmExecutorOpsObjects.cpp (2026-10-27, source-size governance).
    K8 lock classification (ported with the merge): allocation and
    element access run lock-free under the sharing memory model —
    record writes are benign value races on shared records,
    allocation goes through the per-carrier shard free lists, and
    error paths re-acquire inside RaiseNlangException.
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <vector>

namespace nlang {
void VmExecutor::OpAllocArrayN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arrayTypeIdx = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    uint16_t dimsBase = reader.ReadUint16();
    //The dims are rank consecutive int32 slots. Length validation
    //(per-dim negative/cap and the elemCount product) lives in
    //AllocArrayOnHeap — the single choke point shared with native
    //minting.
    int32_t dims[kMaxArrayRank];
    if (rank < 1 || rank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: OP_AllocArrayN rank out of range");
    for (uint16_t d = 0; d < rank; ++d)
        std::memcpy(&dims[d], locals + dimsBase + d * kFrameSlotBytes,
                    sizeof(int32_t));
    int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    NoteGcPressure();
}

void VmExecutor::OpLoadElementN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Element opcodes are only emitted for array-typed bases; any other
    //slot kind is a compiler invariant violation (uncatchable).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN rank mismatch");
    const int32_t* dims = slot.data() + 4;
    //Per-dimension bounds check with the row-major flat index; a named
    //dimension in the message keeps ND debugging humane.
    int64_t flat = 0;
    for (uint16_t d = 0; d < rank; ++d) {
        int32_t idx;
        std::memcpy(&idx, locals + idxBase + d * kFrameSlotBytes,
                    sizeof(idx));
        if (idx < 0 || idx >= dims[d])
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array index out of bounds (dim "
                + std::to_string(d) + ")");
        flat = flat * dims[d] + idx;
    }
    //0.7.5: element stride is registry-driven (slot[1] holds the array
    //type idx recorded at alloc). A 2-cell element copies 8 bytes —
    //the destination frame slot is the uniform 8-byte cell.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(locals + dst,
        slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

void VmExecutor::OpStoreElementN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Same kind gate as OP_LoadElementN (invariant violation, uncatchable).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN rank mismatch");
    const int32_t* dims = slot.data() + 4;
    int64_t flat = 0;
    for (uint16_t d = 0; d < rank; ++d) {
        int32_t idx;
        std::memcpy(&idx, locals + idxBase + d * kFrameSlotBytes,
                    sizeof(idx));
        if (idx < 0 || idx >= dims[d])
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array index out of bounds (dim "
                + std::to_string(d) + ")");
        flat = flat * dims[d] + idx;
    }
    //0.7.5: mirror of OP_LoadElementN's stride read — 2-cell elements
    //copy the full 8 bytes from the source frame slot.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        locals + src, static_cast<size_t>(cells) * sizeof(int32_t));
}

void VmExecutor::OpArrayLength(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Same kind gate as OP_LoadElement (hole 3). No index
    //operand here, so only the range check precedes it; the
    //kind must still gate the slot[2] length read.
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_ArrayLength expects an array slot");
    //Stage-2 header: elemCount moved to slot[3].
    int32_t len = m_structHeap[static_cast<size_t>(heapIdx)][3];
    std::memcpy(locals + dst, &len, sizeof(len));
}

//Stage-2: flat element read — the payload is contiguous row-major, so a
//linear index addresses rank-1 and rank-N arrays alike (foreach walks
//elemCount elements with this). Bounds: flat < elemCount.
void VmExecutor::OpLoadElementFlat(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t flatSlot = reader.ReadUint16();
    int32_t heapIdx, flat;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    std::memcpy(&flat, locals + flatSlot, sizeof(flat));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementFlat expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t elemCount = slot[3];
    if (flat < 0 || flat >= elemCount)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: array index out of bounds");
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementFlat on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    const int32_t rank = slot[2];
    std::memcpy(locals + dst,
        slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

//Stage-2: mint the shape array int[rank] and copy the dims in.
void VmExecutor::OpArrayShape(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_ArrayShape expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t rank = slot[2];
    if (rank < 1 || rank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: OP_ArrayShape on corrupt record rank");
    //Snapshot the dims BEFORE allocating: AllocArrayOnHeap grows
    //m_structHeap and would invalidate the source-record reference
    //(choke-point contract at AllocArrayOnHeap).
    int32_t dimsSnapshot[kMaxArrayRank];
    for (int32_t d = 0; d < rank; ++d)
        dimsSnapshot[d] = slot[4 + d];
    //The shape array is int[rank] — one dim cell per rank; the scalar
    //int[] table entry comes from the mint filter.
    const uint16_t shapeTypeIdx = FindScalarArrayTypeIdx(RTK_Int32, 1);
    int32_t shapeDim = rank;
    int32_t shapeIdx = AllocArrayOnHeap(shapeTypeIdx, 1, &shapeDim);
    NoteGcPressure();
    auto& shape = m_structHeap[static_cast<size_t>(shapeIdx)];
    const int32_t base =
        static_cast<int32_t>(ArrayPayloadBase(shape));
    for (int32_t d = 0; d < rank; ++d)
        shape[base + d] = dimsSnapshot[d];
    std::memcpy(locals + dst, &shapeIdx, sizeof(shapeIdx));
}

//Stage-3 D3: assemble a rank>=2 array from staged row handles - the
//dynamic-shape ND initializer arm (OP_AssembleArrayN). Rows validate
//and snapshot in SnapshotAssembleRows; this allocates the record and
//copies the row payloads into its contiguous row-major storage. Only
//struct elements deep-copy (value semantics); every other kind copies
//cell-wise, exactly like OP_StoreElementN's slot write.
void VmExecutor::OpAssembleArrayN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arrayTypeIdx = reader.ReadUint16();
    uint16_t elemSlotBase = reader.ReadUint16();
    uint16_t rowCount = reader.ReadUint16();
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_AssembleArrayN on invalid array type");
    const CompiledArrayType& arrayType =
        m_currModule->arrayTypes[arrayTypeIdx];
    const uint8_t rank = arrayType.rank;
    //The emitter only produces rank>=2 assemblies with non-empty row
    //lists; anything else is a compiler invariant violation.
    if (rank < 2 || rank > kMaxArrayRank || rowCount == 0)
        throw std::runtime_error(
            "NLang VM: OP_AssembleArrayN on invalid shape header");
    std::vector<int32_t> rowHandles(rowCount);
    int32_t dims[kMaxArrayRank];
    SnapshotAssembleRows(locals, elemSlotBase, rowCount, rank,
                         rowHandles, dims);
    const int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx, rank, dims);
    NoteGcPressure();
    CopyAssembleRows(heapIdx, arrayType, rowHandles);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
}

//Row-payload copy of OpAssembleArrayN: every row's cells block-copy
//into the fresh record's contiguous row-major storage. Only struct
//elements deep-copy (value semantics); every other kind copies
//cell-wise, exactly like OP_StoreElementN's slot write.
void VmExecutor::CopyAssembleRows(int32_t heapIdx,
    const CompiledArrayType& arrayType,
    const std::vector<int32_t>& rowHandles) {
    const uint8_t rank = arrayType.rank;
    const size_t destBase = ArrayHeaderCells(rank);
    const size_t rowBase = ArrayHeaderCells(static_cast<uint8_t>(rank - 1));
    const size_t cells = static_cast<size_t>(
        ArrayElemCells(arrayType.elemKind));
    const bool structElems = arrayType.elemKind == RTK_Struct
        && arrayType.elemTypeIdx != 0xFFFF;
    const size_t rowCount = rowHandles.size();
    for (size_t r = 0; r < rowCount; ++r) {
        const size_t rowSlot = static_cast<size_t>(rowHandles[r]);
        const size_t rowElems =
            static_cast<size_t>(m_structHeap[rowSlot][3]);
        if (!structElems) {
            const int32_t* rowCells = m_structHeap[rowSlot].data();
            auto& dest = m_structHeap[static_cast<size_t>(heapIdx)];
            std::memcpy(dest.data() + destBase + r * rowElems * cells,
                rowCells + rowBase, rowElems * cells * sizeof(int32_t));
            continue;
        }
        //Struct elements are values: one DeepCopyStruct per element.
        //Each deep copy grows m_structHeap, so every access re-indexes
        //by the stable heap handles instead of holding a reference.
        for (size_t e = 0; e < rowElems; ++e) {
            const int32_t elemHandle =
                m_structHeap[rowSlot][rowBase + e];
            const int32_t copyIdx = DeepCopyStruct(elemHandle,
                                                   arrayType.elemTypeIdx);
            m_structHeap[static_cast<size_t>(heapIdx)]
                [destBase + r * rowElems + e] = copyIdx;
        }
    }
}

//Validation pass of OpAssembleArrayN: null/kind/row-rank gates plus the
//shared per-level width check, and the handle/dims snapshot the
//allocation consumes. Target dim L (L >= 1) is row dim L-1; the first
//row pins it, later rows must agree - differing widths are user data
//shape and take the catchable base-exception family.
void VmExecutor::SnapshotAssembleRows(const uint8_t* locals,
    uint16_t elemSlotBase, uint16_t rowCount, uint8_t rank,
    std::vector<int32_t>& rowHandles, int32_t* dims) {
    dims[0] = static_cast<int32_t>(rowCount);
    for (uint16_t r = 0; r < rowCount; ++r) {
        std::memcpy(&rowHandles[r],
                    locals + elemSlotBase + r * kFrameSlotBytes,
                    sizeof(int32_t));
        const int32_t handle = rowHandles[r];
        if (handle <= 0
            || static_cast<size_t>(handle) >= m_structHeap.size())
            RaiseNlangException(m_nullPtrExcClassIdx,
                                "NLang VM: null array access");
        if (m_slotKinds[static_cast<size_t>(handle)] != RTK_Array)
            throw std::runtime_error(
                "NLang VM: OP_AssembleArrayN expects array rows");
        auto& row = m_structHeap[static_cast<size_t>(handle)];
        if (row[2] != rank - 1)
            throw std::runtime_error(
                "NLang VM: OP_AssembleArrayN row rank mismatch");
        for (uint8_t d = 0; d + 1 < rank; ++d) {
            if (r == 0)
                dims[d + 1] = row[4 + d];
            else if (row[4 + d] != dims[d + 1])
                RaiseNlangExceptionBase(
                    "ND initializer widths differ (dim "
                    + std::to_string(d + 1) + ": "
                    + std::to_string(dims[d + 1]) + " vs "
                    + std::to_string(row[4 + d]) + ")");
        }
    }
}

} // namespace nlang
