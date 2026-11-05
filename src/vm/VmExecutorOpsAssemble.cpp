/*---
    VmExecutorOpsAssemble.cpp - the dynamic-shape ND initializer arm:
    OP_AssembleArrayN and its row validation/copy passes. Split from
    VmExecutorOpsArrayN.cpp when that file reached the source-size
    budget (2026-11-05, source-size governance). Rows may be owning
    records or view records (Task 10): owning rows block-copy, view
    rows walk the affine map into the owner's payload.
    K8 lock classification (ported with the split): lock-free under the
    sharing memory model — record writes are benign value races on
    shared records, allocation goes through the per-carrier shard free
    lists, and error paths re-acquire inside RaiseNlangException.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <vector>

namespace nlang {

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

//Row-payload copy of OpAssembleArrayN: every row's cells copy into the
//fresh record's contiguous row-major storage. Owning rows block-copy;
//view rows walk the affine map element-wise through
// CopyAssembleViewRow (their payload lives in the owner, not at their
//own header). Only struct elements deep-copy (value semantics); every
//other kind copies cell-wise, exactly like OP_StoreElementN's slot
//write.
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
        if (m_slotKinds[rowSlot] == RTK_ArrayView) {
            CopyAssembleViewRow(heapIdx, rowSlot, rowElems, destBase, r,
                                arrayType, cells, structElems);
            continue;
        }
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

//View-row copy of OpAssembleArrayN: the row's elements sit in its
//owner's payload, so every element runs through the affine map
//(ViewFlatElementAddress) instead of a block copy. Struct elements
//deep-copy like the owned-row arm — each deep copy grows m_structHeap,
//so every access re-indexes by the stable heap handles (the owned-row
//arm pays the same discipline).
void VmExecutor::CopyAssembleViewRow(int32_t heapIdx, size_t rowSlot,
    size_t rowElems, size_t destBase, size_t rowOrdinal,
    const CompiledArrayType& arrayType, size_t cells, bool structElems) {
    size_t srcBase;
    ElementPayloadRecord(m_structHeap[rowSlot], RTK_ArrayView, srcBase);
    const uint8_t rowRank =
        static_cast<uint8_t>(m_structHeap[rowSlot][2]);
    const size_t ownerIdx = static_cast<size_t>(
        m_structHeap[rowSlot][ArrayViewTargetCell(rowRank)]);
    for (size_t e = 0; e < rowElems; ++e) {
        const int64_t addr = ViewFlatElementAddress(
            m_structHeap[rowSlot], static_cast<int64_t>(e));
        if (!structElems) {
            std::memcpy(
                m_structHeap[static_cast<size_t>(heapIdx)].data()
                    + destBase + (rowOrdinal * rowElems + e) * cells,
                m_structHeap[ownerIdx].data() + srcBase
                    + static_cast<size_t>(addr) * cells,
                cells * sizeof(int32_t));
            continue;
        }
        const int32_t elemHandle =
            m_structHeap[ownerIdx][srcBase + static_cast<size_t>(addr)];
        const int32_t copyIdx = DeepCopyStruct(elemHandle,
            arrayType.elemTypeIdx);
        m_structHeap[static_cast<size_t>(heapIdx)]
            [destBase + rowOrdinal * rowElems + e] = copyIdx;
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
        if (m_slotKinds[static_cast<size_t>(handle)] != RTK_Array
            && m_slotKinds[static_cast<size_t>(handle)] != RTK_ArrayView)
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
