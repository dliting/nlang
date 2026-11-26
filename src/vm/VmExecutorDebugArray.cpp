/*---
    VmExecutorDebugArray.cpp — the array-family debugger display
    (owning records and views, v0.12.7 Task 13). Split from
    VmExecutorDebug.cpp for the file-size guard; the entry points and
    constants stay in VmExecutorDebug.cpp.
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <string>

namespace nlang {

//Rank-notation short tag for a nested view reference ("<double[,]
//view>") — the owning arm's counterpart shows the flat count; a view's
//identity is its rank (the full display below shows the geometry).
std::string VmExecutor::DebugViewShortTag(
    const std::vector<int32_t>& view) const {
    uint16_t arrayTypeIdx = static_cast<uint16_t>(view[1]);
    uint8_t elemKind = RTK_Int32;
    if (m_currModule
        && arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    std::string tag = "<" + std::string(DebugKindName(elemKind)) + "[";
    for (int32_t d = 1; d < view[2]; ++d)
        tag += ",";
    return tag + "] view>";
}

std::string VmExecutor::FormatDebugArray(int32_t heapIdx) const {
    //v0.14.x device line: stage a device record's payload before the
    //element loop (one bulk D2H through the shared staging arm — the
    //same primitive the print path uses; the debugger reads the staged
    //cells like any host record).
    std::vector<int32_t> stagedSlot;
    StageDeviceArrayForFormat(heapIdx, stagedSlot);
    const auto& slot = stagedSlot.empty()
        ? static_cast<const std::vector<int32_t>&>(
            m_structHeap[static_cast<size_t>(heapIdx)])
        : stagedSlot;
    uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    int32_t length = slot[3];  //stage-2 header: elemCount
    uint8_t elemKind = RTK_Int32;
    if (m_currModule
        && arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    std::string result = std::string(DebugKindName(elemKind)) + "["
        + std::to_string(length) + "]{";
    int bound = length < kMaxElementsShown ? length : kMaxElementsShown;
    //0.7.5: registry stride — 8-byte scalar elements read the cell
    //pair through the same stride-aware field renderer as everyone
    //else; 1-cell kinds read their single cell.
    const int cells = ArrayElemCells(elemKind);
    for (int32_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugField(slot,
            ArrayPayloadBase(slot) + static_cast<size_t>(i) * cells,
            elemKind);
    }
    if (length > bound) result += ", ...";
    result += "}";
    return result;
}

//v0.12.7 Task 13: view-record display. The owning formatter shows the
//flat element count; a view is its geometry — dims in the shape, then
//the strides/offset pair in a parenthesized suffix — with elements
//resolved through the owner along the affine map. The const path
//peels the target directly: a view record's target is always an owned
//record (the depth-one invariant), so no payload-base negotiation is
//needed.
std::string VmExecutor::FormatDebugViewArray(int32_t heapIdx) const {
    const auto& view = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t arrayTypeIdx = static_cast<uint16_t>(view[1]);
    const uint8_t rank = static_cast<uint8_t>(view[2]);
    uint8_t elemKind = RTK_Int32;
    if (m_currModule
        && arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    std::string result = std::string(DebugKindName(elemKind)) + "[";
    for (uint8_t d = 0; d < rank; ++d)
        result += (d == 0 ? "" : ", ") + std::to_string(view[4 + d]);
    result += "] view(strides=[";
    for (uint8_t d = 0; d < rank; ++d)
        result += (d == 0 ? "" : ", ")
            + std::to_string(view[ArrayViewStridesBase(rank) + d]);
    result += "], offset=" + std::to_string(view[ArrayViewOffsetCell(rank)])
        + "){";
    const int32_t length = view[3];
    int bound = length < kMaxElementsShown ? length : kMaxElementsShown;
    const int cells = ArrayElemCells(elemKind);
    //v0.14.x device line: a device-resident owner carries its payload in
    //the 2-cell device-pointer descriptor — stage it before the affine
    //walk (the display twin of the print path's FormatViewArray arm;
    //unstaged reads show pointer fragments and run past the record end).
    const int32_t target = view[ArrayViewTargetCell(rank)];
    std::vector<int32_t> stagedOwner;
    if (target > 0
        && static_cast<size_t>(target) < m_arrResidency.size())
        StageDeviceArrayForFormat(target, stagedOwner);
    const auto& owner = stagedOwner.empty()
        ? static_cast<const std::vector<int32_t>&>(
            m_structHeap[static_cast<size_t>(target)])
        : stagedOwner;
    const size_t ownerBase = ArrayPayloadBase(owner);
    for (int32_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        const int64_t addr = ViewFlatElementAddress(view, i);
        result += FormatDebugField(owner,
            ownerBase + static_cast<size_t>(addr) * cells, elemKind);
    }
    if (length > bound) result += ", ...";
    result += "}";
    return result;
}

} // namespace nlang
