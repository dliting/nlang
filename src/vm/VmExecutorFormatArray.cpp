/*---
    VmExecutorFormatArray.cpp — 数组/视图 toString 渲染族
    从 VmExecutorFormat.cpp 抽取（2026-11-16 P4 source-size收口，
    零行为变化）：数组元素与ND嵌套渲染、视图affine映射渲染。
---*/
#include "VmExecutor.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace nlang {

//One 1-cell array element by kind. Scalars delegate to the canonical
//renderer (which reads the carrier's own width from the low bytes);
//reference kinds delegate to the heap renderer.
std::string VmExecutor::FormatArrayElemScalar(int32_t elemVal,
    uint8_t elemKind, int depth) {
    switch (elemKind) {
    case RTK_String:
        return QuoteString(StrVal(elemVal));
    case RTK_Struct:
        return "<struct>";
    case RTK_Class:
    case RTK_Boxed:
    case RTK_Array:
        return FormatHeapValue(elemVal, depth);
    default:
        return FormatScalarValue(elemKind, elemVal, 0);
    }
}

//One array element's toString contribution. 0.7.5: element stride is
//registry-driven — 8-byte scalars occupy two cells (off..off+1) and
//render through the canonical scalar renderer like everyone else;
//every other kind is 1-cell.
void VmExecutor::FormatArrayElement(std::string& result,
    const std::vector<int32_t>& slot, size_t off, uint8_t elemKind,
    int cells, int depth) {
    if (cells == 2) {
        result += FormatScalarValue(elemKind, slot[off], slot[off + 1]);
        return;
    }
    result += FormatArrayElemScalar(slot[off], elemKind, depth);
}

//One nesting level of ND formatting: the level's dim partitions the
//remaining flat payload; the leaf level formats elements at the
//running flat index (FormatArrayElement handles the per-kind cells).
void VmExecutor::FormatArrayNDLevel(std::string& result,
    const std::vector<int32_t>& slot, uint8_t rank, uint8_t level,
    size_t& flatIndex, uint8_t elemKind, int cells, int depth) {
    result += "[";
    const int32_t n = slot[4 + level];
    const size_t base = ArrayPayloadBase(slot);
    for (int32_t i = 0; i < n; ++i) {
        if (i > 0) result += ", ";
        if (level + 1 < rank) {
            FormatArrayNDLevel(result, slot, rank,
                static_cast<uint8_t>(level + 1), flatIndex, elemKind,
                cells, depth);
        } else {
            FormatArrayElement(result, slot,
                base + flatIndex * static_cast<size_t>(cells), elemKind,
                cells, depth);
            ++flatIndex;
        }
    }
    result += "]";
}

std::string VmExecutor::FormatArray(int32_t heapIdx, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    //A view record's payload lives in its owner — the bracket walk is
    //the same, the element reads are not (affine map, owner window).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_ArrayView)
        return FormatViewArray(heapIdx, depth);
    //v0.14.x device line: a device-resident record's payload area is
    //the 2-cell device-pointer descriptor — stage the real payload
    //into a record-shaped local before the walk (one bulk D2H).
    std::vector<int32_t> stagedSlot;
    StageDeviceArrayForFormat(heapIdx, stagedSlot);
    const std::vector<int32_t>& slot = stagedSlot.empty()
        ? static_cast<const std::vector<int32_t>&>(
            m_structHeap[static_cast<size_t>(heapIdx)])
        : stagedSlot;
    uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    int32_t length = slot[3];
    uint8_t elemKind = RTK_Int32;
    if (arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    //Stage-2 ND: rank>=2 records carry the dims in the header over one
    //flat row-major payload — the brackets nest per dim level, and a
    //zero dim still shows its nesting (int[3,0] is "[[], [], []]").
    const uint8_t rank = static_cast<uint8_t>(slot[2]);
    if (rank >= 2) {
        std::string result;
        size_t flatIndex = 0;
        FormatArrayNDLevel(result, slot, rank, 0, flatIndex, elemKind,
            ArrayElemCells(elemKind), depth);
        return result;
    }
    if (length <= 0) return "[]";
    std::string result = "[";
    const int cells = ArrayElemCells(elemKind);
    for (int32_t i = 0; i < length; ++i) {
        if (i > 0) result += ", ";
        FormatArrayElement(result, slot,
            ArrayPayloadBase(slot) + static_cast<size_t>(i) * cells,
            elemKind, cells, depth);
    }
    result += "]";
    return result;
}

//v0.12.7 Task 10: view-record formatting entry — the element kind comes
//from the view's own type row (creation validated it against the
//owner's), and the bracket walk runs over the view's own dims.
std::string VmExecutor::FormatViewArray(int32_t heapIdx, int depth) {
    //v0.14.x device line: a view over a device-resident owner formats
    //through a transient host materialization (the serializer's own
    //pattern — the copy is written to a string and dropped; numeric
    //device elements carry no class toString side effects).
    {
        const auto& view = m_structHeap[static_cast<size_t>(heapIdx)];
        const int32_t target = view[ArrayViewTargetCell(
            static_cast<uint8_t>(view[2]))];
        if (target > 0
            && static_cast<size_t>(target) < m_arrResidency.size()
            && m_arrResidency[static_cast<size_t>(target)] == 1) {
            return FormatArray(MaterializeViewRecord(heapIdx), depth);
        }
    }
    const auto& view = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t arrayTypeIdx = static_cast<uint16_t>(view[1]);
    uint8_t elemKind = RTK_Int32;
    if (arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    std::string result;
    FormatViewNDLevel(result, view,
        static_cast<uint8_t>(view[2]), 0, 0, heapIdx,
        elemKind, ArrayElemCells(elemKind), depth);
    return result;
}

//One nesting level of view formatting. flat is the row-major ordinal of
//the level's first element over the view's own dims; a child at slot i
//of this level starts flat + i * (product of the deeper dims). Leaf
//reads resolve the owner window fresh per element — an element's own
//formatting (a user toString on a class element) may allocate and grow
//m_structHeap, so no reference survives across leaves.
void VmExecutor::FormatViewNDLevel(std::string& result,
    const std::vector<int32_t>& view, uint8_t rank, uint8_t level,
    int32_t flat, int32_t viewHeapIdx, uint8_t elemKind, int cells,
    int depth) {
    result += "[";
    const int32_t n = view[4 + level];
    int32_t tail = 1;
    for (uint8_t d = level + 1; d < rank; ++d)
        tail *= view[4 + d];
    for (int32_t i = 0; i < n; ++i) {
        if (i > 0) result += ", ";
        if (level + 1 < rank) {
            FormatViewNDLevel(result, view, rank,
                static_cast<uint8_t>(level + 1), flat + i * tail,
                viewHeapIdx, elemKind, cells, depth);
            continue;
        }
        size_t base;
        auto& owner = ElementPayloadRecord(
            m_structHeap[static_cast<size_t>(viewHeapIdx)],
            RTK_ArrayView, base);
        const int64_t addr = ViewFlatElementAddress(view, flat + i);
        FormatArrayElement(result, owner,
            base + static_cast<size_t>(addr) * cells,
            elemKind, cells, depth);
    }
    result += "]";
}

} // namespace nlang
