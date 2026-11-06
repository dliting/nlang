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

//Dual-shape element address shared by the element op family: per-dim
//bounds check against the record's own dims (catchable OOB naming the
//dimension keeps ND debugging humane), then the flat index in OWNER
//payload space. An owning record folds computed row-major strides from
//offset 0; a view record folds its recorded strides from its recorded
//offset (negative strides reverse, zero strides broadcast). The caller
//has already gated null/kind and the rank match.
int64_t VmExecutor::ElementFlatAddress(const std::vector<int32_t>& slot,
    uint8_t kind, uint16_t rank, uint16_t idxBase, uint8_t* locals) {
    const int32_t* dims = slot.data() + 4;
    int32_t rowMajor[kMaxArrayRank];
    const int32_t* strides;
    int64_t flat;
    if (kind == RTK_ArrayView) {
        strides = slot.data() + ArrayViewStridesBase(
            static_cast<uint8_t>(rank));
        flat = slot[ArrayViewOffsetCell(static_cast<uint8_t>(rank))];
    } else {
        int32_t stride = 1;
        for (int32_t d = rank - 1; d >= 0; --d) {
            rowMajor[d] = stride;
            stride *= dims[d];
        }
        strides = rowMajor;
        flat = 0;
    }
    for (uint16_t d = 0; d < rank; ++d) {
        int32_t idx;
        std::memcpy(&idx, locals + idxBase + d * kFrameSlotBytes,
                    sizeof(idx));
        if (idx < 0 || idx >= dims[d])
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array index out of bounds (dim "
                + std::to_string(d) + ")");
        flat += static_cast<int64_t>(idx) * strides[d];
    }
    return flat;
}

//Dual-shape payload window: owning records carry the payload at their
//own padded header, view records address their OWNER's payload (the
//depth-one peel invariant puts an owned record handle in the target
//cell). Callers must not hold the returned reference across a
//heap-growing call.
std::vector<int32_t>& VmExecutor::ElementPayloadRecord(
    std::vector<int32_t>& slot, uint8_t kind, size_t& payloadBaseOut) {
    if (kind != RTK_ArrayView) {
        payloadBaseOut = ArrayPayloadBase(slot);
        return slot;
    }
    const uint8_t rank = static_cast<uint8_t>(slot[2]);
    const int32_t target = slot[ArrayViewTargetCell(rank)];
    if (target <= 0
        || static_cast<size_t>(target) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(target)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: corrupt view record (target out of range)");
    auto& owner = m_structHeap[static_cast<size_t>(target)];
    payloadBaseOut = ArrayPayloadBase(owner);
    return owner;
}

//Affine address of the flat-eth element (row-major over the record's
//own dims) of a VIEW record, in owner payload space — the coordinate
//decode foreach and row copies share. dims[d] >= 1 holds by creation
//validation, so the div/mod walk is exact.
int64_t VmExecutor::ViewFlatElementAddress(
    const std::vector<int32_t>& view, int64_t flat) const {
    const int32_t rank = view[2];
    const int32_t* dims = view.data() + 4;
    const int32_t* strides =
        view.data() + ArrayViewStridesBase(static_cast<uint8_t>(rank));
    int64_t addr = view[ArrayViewOffsetCell(static_cast<uint8_t>(rank))];
    int64_t rem = flat;
    for (int32_t d = rank - 1; d >= 0; --d) {
        const int32_t coord = static_cast<int32_t>(rem % dims[d]);
        rem /= dims[d];
        addr += static_cast<int64_t>(coord) * strides[d];
    }
    return addr;
}

void VmExecutor::RejectZeroStrideStore(const std::vector<int32_t>& slot,
                                       uint8_t rank) {
    const int32_t* dims = slot.data() + 4;
    const int32_t* strides =
        slot.data() + ArrayViewStridesBase(rank);
    for (uint8_t d = 0; d < rank; ++d) {
        //The rejection is derived from the record itself — a zero-stride
        //axis only aliases when it spans more than one element (several
        //coordinates fold onto one owner cell). A size-1 or empty axis
        //addresses one cell through any stride, so its store stays
        //legal whatever stride the producer recorded; encoding choices
        //upstream can never lock a legal write out.
        if (dims[d] > 1 && strides[d] == 0)
            RaiseNlangExceptionBase(
                "cannot store through a broadcast (zero-stride) view; "
                "reshape it to materialize a writable copy");
    }
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
    //Element opcodes are only emitted for array-typed bases, and both
    //record kinds are array-typed values; any other slot kind is a
    //compiler invariant violation (uncatchable).
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN rank mismatch");
    int64_t flat = ElementFlatAddress(slot, kind, rank, idxBase, locals);
    size_t payloadBase;
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    //0.7.5: element stride is registry-driven (slot[1] holds the array
    //type idx recorded at alloc; a view's own row carries the same
    //element kind — creation validated it against the owner). A 2-cell
    //element copies 8 bytes — the destination frame slot is the
    //uniform 8-byte cell.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(locals + dst,
        payload.data() + payloadBase
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
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN rank mismatch");
    //Broadcast axes reject the store up front (one pass, all axes):
    //every logical index folds onto one owner cell, so a store would
    //silently corrupt aliases.
    if (kind == RTK_ArrayView)
        RejectZeroStrideStore(slot, static_cast<uint8_t>(rank));
    int64_t flat = ElementFlatAddress(slot, kind, rank, idxBase, locals);
    size_t payloadBase;
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    //0.7.5: mirror of OP_LoadElementN's stride read — 2-cell elements
    //copy the full 8 bytes from the source frame slot.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(payload.data() + payloadBase
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
    //kind must still gate the slot[2] length read. A view's
    //elemCount is its own logical count.
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
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
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
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
    //A view's payload is not contiguous: decode the linear index to
    //coordinates over the view's own dims, then apply the affine map.
    const int64_t addr = kind == RTK_ArrayView
        ? ViewFlatElementAddress(slot, flat)
        : static_cast<int64_t>(flat);
    size_t payloadBase;
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    std::memcpy(locals + dst,
        payload.data() + payloadBase
            + static_cast<size_t>(addr) * cells,
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
    //A view reports its own logical dims (the shared header prefix
    //makes the read kind-agnostic).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
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

//v0.12.7 Task 4: the unannotated Array<E> spelling's element read. The
//record rank is unknown to the compiler, so the source-written
//coordinate count verifies against it here; a mismatch is a USER error
//(catchable OOB carrying both counts), unlike the static pair's
//invariant. Views dispatch through the same dual-shape helpers as the
//static pair (Task 10).
void VmExecutor::OpLoadElementDyn(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t nCoords = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementDyn expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != nCoords)
        RaiseNlangException(m_oobExcClassIdx,
            "array subscript expects " + std::to_string(recRank)
            + " indices, got " + std::to_string(nCoords)
            + " (rank " + std::to_string(recRank) + ")");
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementDyn on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    int64_t flat = ElementFlatAddress(slot, kind, nCoords, idxBase, locals);
    size_t payloadBase;
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    std::memcpy(locals + dst,
        payload.data() + payloadBase
            + static_cast<size_t>(flat) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

//Store twin of OpLoadElementDyn (same gates, payload copy reversed);
//view bases take the broadcast-axis rejection like the static store.
void VmExecutor::OpStoreElementDyn(BytecodeReader& reader, uint8_t* locals) {
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t nCoords = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementDyn expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != nCoords)
        RaiseNlangException(m_oobExcClassIdx,
            "array subscript expects " + std::to_string(recRank)
            + " indices, got " + std::to_string(nCoords)
            + " (rank " + std::to_string(recRank) + ")");
    if (kind == RTK_ArrayView)
        RejectZeroStrideStore(slot, static_cast<uint8_t>(nCoords));
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_StoreElementDyn on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    int64_t flat = ElementFlatAddress(slot, kind, nCoords, idxBase, locals);
    size_t payloadBase;
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    std::memcpy(payload.data() + payloadBase
            + static_cast<size_t>(flat) * cells,
        locals + src, static_cast<size_t>(cells) * sizeof(int32_t));
}

//v0.12.7 Task 4: `Array(dims)` — the dims handle is a rank-1 int[]; the
//record rank is its length (validated [1, kMaxArrayRank]) and the
//extents are its payload. Length/extent validation funnels through
//AllocArrayOnHeap, the single choke point shared with static creation.
void VmExecutor::OpAllocArrayDyn(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arrayTypeIdx = reader.ReadUint16();
    uint16_t dimsSlot = reader.ReadUint16();
    int32_t dimsIdx;
    std::memcpy(&dimsIdx, locals + dimsSlot, sizeof(dimsIdx));
    if (dimsIdx <= 0 || static_cast<size_t>(dimsIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    if (m_slotKinds[static_cast<size_t>(dimsIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_AllocArrayDyn expects an array slot");
    auto& dimsRec = m_structHeap[static_cast<size_t>(dimsIdx)];
    if (dimsRec[2] != 1)
        RaiseNlangException(m_oobExcClassIdx,
            "Array(dims): the dims argument must be a one-dimensional array");
    const int32_t rank = dimsRec[3];
    if (rank < 1 || rank > kMaxArrayRank)
        RaiseNlangException(m_oobExcClassIdx,
            "Array(dims): rank " + std::to_string(rank) + " out of range");
    //The resolver pins the dims element to int32 (4-byte scalar), so the
    //payload stride is 1 cell — read the extents row-major.
    const int cells = ArrayElemCells(RTK_Int32);
    int32_t dims[kMaxArrayRank];
    for (int32_t d = 0; d < rank; ++d)
        std::memcpy(&dims[d],
            dimsRec.data() + ArrayHeaderCells(1) + d * cells,
            sizeof(int32_t));
    int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    NoteGcPressure();
}

} // namespace nlang
