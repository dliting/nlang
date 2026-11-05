/*---
    VmExecutorOpsArrayCheck.cpp — the array boundary-check opcode family
    (v0.12.7): the annotated-target rank check (Task 3) and the
    Object→array downcast type check (Task 5). Split from
    VmExecutorOpsArrayN.cpp when the family grew past that file's size
    budget; both handlers raise the catchable InvalidCastException.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <stdexcept>

namespace nlang {

//v0.12.7 Task 3: boundary rank check — the runtime half of the promise
//model. An annotated target (rank >= 1) receiving an unannotated value
//(Array<E> spelling, or a value flowed through Object) verifies the
//record's rank. Null is a legal value of every array type (spec 4.1)
//and passes through unchanged; a non-array handle or a rank mismatch
//raises the catchable InvalidCastException.
void VmExecutor::OpCheckArrayRank(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t expect = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    if (heapIdx == 0) {
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_CheckArrayRank on invalid reference");
    //Views carry the same header prefix (rank at [2]) — the shared-prefix
    //layout is what makes this check shape-blind (v0.12.7 Task 8).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array cast: value is not an array");
    const int32_t got = m_structHeap[static_cast<size_t>(heapIdx)][2];
    if (static_cast<uint16_t>(got) != expect)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array rank mismatch: expected "
            + std::to_string(expect) + ", got " + std::to_string(got));
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
}

//v0.12.7 Task 5: the Object→array downcast check — `o as int[]`. The
//arrayTypes table dedups per merged image at link time (NcuLinker's
//MergeArrayTypes), so record type identity IS type identity: one
//comparison verifies rank and element type together. Null passes
//through; a non-array handle or a row mismatch raises the catchable
//InvalidCastException (the message names rank vs element type).
void VmExecutor::OpCheckArrayType(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t expectIdx = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    if (heapIdx == 0) {
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_CheckArrayType on invalid reference");
    //Views: same accepted kind pair as OpCheckArrayRank above — the
    //typeIdx/rank cells this check reads sit at identical offsets on both
    //layouts (v0.12.7 Task 8).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
        && m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_ArrayView)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array cast: value is not an array");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const auto gotIdx = static_cast<uint16_t>(slot[1]);
    if (gotIdx == expectIdx) {
        std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
        return;
    }
    //Precise diagnostics: a rank mismatch names both ranks; same-rank
    //failures are element types.
    const auto& rows = m_currModule->arrayTypes;
    const auto gotRank = static_cast<uint8_t>(slot[2]);
    if (expectIdx < rows.size() && gotRank != rows[expectIdx].rank)
        RaiseNlangException(m_invalidCastExcClassIdx,
            "array rank mismatch: expected "
            + std::to_string(rows[expectIdx].rank) + ", got "
            + std::to_string(static_cast<int>(gotRank)));
    RaiseNlangException(m_invalidCastExcClassIdx,
        "array element type mismatch");
}

} // namespace nlang
