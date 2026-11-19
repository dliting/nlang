/*---
    VmExecutorDebugSlots.cpp — the byref/promoted slot display
    (v0.13.9 phase 3 Task 11): element-kind resolution plus the
    "&#handle[cell] -> value" formatters. Split from
    VmExecutorDebug.cpp for the file-size guard.
---*/
#include "VmExecutor.h"
#include <cstring>
#include <string>

namespace nlang {

namespace {

//Unresolved-kind sentinel for the display kind resolution (0xFF follows
//RtkOfKind's "not a kind" convention; RTK_Int32 is 0, so 0 cannot say
//"unresolved").
constexpr uint8_t kDebugKindUnresolved = 0xFF;

} //namespace

//The cell-path arm of the kind resolution (below): the declared kind of
//one cell, read through the base record's own metadata — box tag, struct
//field row, class layout field, array element kind.
uint8_t VmExecutor::ByrefCellKindOf(size_t h, size_t cell) const {
    const auto& record = m_structHeap[h];
    switch (m_slotKinds[h]) {
    case RTK_Boxed:
        //The tag doubles as the declared kind of the payload cells.
        return static_cast<uint8_t>(record[0]);
    case RTK_Struct: {
        const uint16_t structIdx = m_slotStructIdx[h];
        if (!m_currModule
                || structIdx >= m_currModule->structs.size())
            return kDebugKindUnresolved;
        const auto& cs = m_currModule->structs[structIdx];
        const size_t field = cell / 2;  //struct fields are 2 cells wide
        return field < cs.fieldCount
            ? static_cast<uint8_t>(cs.fieldTypeKinds[field])
            : kDebugKindUnresolved;
    }
    case RTK_Class: {
        if (!m_currModule || record.empty())
            return kDebugKindUnresolved;
        const int32_t classIdx = record[0];
        if (classIdx < 0
                || static_cast<size_t>(classIdx)
                    >= m_currModule->classes.size())
            return kDebugKindUnresolved;
        const auto& fields = m_classLayout.Fields(
            static_cast<size_t>(classIdx));
        const size_t field = cell >= 1 ? (cell - 1) / 2 : 0;
        return field < fields.size()
            ? static_cast<uint8_t>(fields[field].kind)
            : kDebugKindUnresolved;
    }
    case RTK_Array: {
        if (!m_currModule || record.size() < 2)
            return kDebugKindUnresolved;
        const uint16_t arrayTypeIdx = static_cast<uint16_t>(record[1]);
        if (arrayTypeIdx >= m_currModule->arrayTypes.size())
            return kDebugKindUnresolved;
        return m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    }
    default:
        return kDebugKindUnresolved;
    }
}

//The element kind one byref/promoted slot addresses. The promoted slot
//reads only its box handle (the upper half is stale) and names the box
//tag. A byref pair reads the whole pair: the AGGREGATE family names the
//base record itself at offset 0 (the D9 convention — a whole-struct
//reference); every other address resolves through the base family's own
//metadata — which is what makes a first-field scalar & readable: its
//cell-0 pair resolves through the struct's field row instead of the
//record. Shared by the display and the kindName composition; never
//throws — a malformed slot (scope-visible but unexecuted binding,
//corrupt record) yields the sentinel and the caller renders
//"null"/"unknown&".
uint8_t VmExecutor::ByrefElementKindOf(const uint8_t* frameLocals,
    uint16_t offset, uint8_t slotKind) const {
    int32_t handle;
    std::memcpy(&handle, frameLocals + offset, sizeof(handle));
    if (handle <= 0
            || static_cast<size_t>(handle) >= m_structHeap.size())
        return kDebugKindUnresolved;
    const size_t h = static_cast<size_t>(handle);
    if (slotKind == RTK_Promoted) {
        const uint8_t baseKind = m_slotKinds[h];
        return baseKind == RTK_Boxed
            ? static_cast<uint8_t>(m_structHeap[h][0]) : baseKind;
    }
    int32_t cellOff;
    std::memcpy(&cellOff, frameLocals + offset + sizeof(cellOff),
                sizeof(cellOff));
    if (slotKind == RTK_Byref && cellOff == 0)
        return m_slotKinds[h];
    if (cellOff < 0
            || static_cast<size_t>(cellOff) >= m_structHeap[h].size())
        return kDebugKindUnresolved;
    return ByrefCellKindOf(h, static_cast<size_t>(cellOff));
}

//A byref slot renders the pair: "&#<handle>[<cell>] -> <value>". The
//raw pair is the debugging aid (which record, which cell); the arrow
//carries the dereferenced value so the reference reads like its target.
//The aggregate family renders an offset-0 pair as the whole record; the
//cell family always renders the addressed cell (a cell-0 struct pair is
//first-field storage, not the record).
std::string VmExecutor::FormatDebugByrefSlot(const uint8_t* frameLocals,
    uint16_t offset, uint8_t slotKind) const {
    int32_t pair[2];
    std::memcpy(pair, frameLocals + offset, sizeof(pair));
    if (pair[0] <= 0
            || static_cast<size_t>(pair[0]) >= m_structHeap.size())
        return "null";
    const size_t handle = static_cast<size_t>(pair[0]);
    const auto& record = m_structHeap[handle];
    const std::string head = "&#" + std::to_string(pair[0])
        + "[" + std::to_string(pair[1]) + "] -> ";
    if (slotKind == RTK_Byref && pair[1] == 0)
        return head + FormatDebugHeapValue(static_cast<int32_t>(handle));
    const size_t cell = static_cast<size_t>(pair[1]);
    if (pair[1] < 0 || cell >= record.size())
        return head + "<unknown>";
    const uint8_t elemKind = ByrefElementKindOf(frameLocals, offset,
        slotKind);
    if (elemKind == kDebugKindUnresolved)
        return head + "<unknown>";
    return head + FormatDebugField(record, cell, elemKind);
}

//A promoted slot renders the plain VALUE — the box is an implementation
//detail of aliasing, the user declared a scalar/string. Only the low
//half is read: the upper half holds stale bytes the prologue's Assign
//left behind.
std::string VmExecutor::FormatDebugPromotedSlot(
    const uint8_t* frameLocals, uint16_t offset) const {
    int32_t handle;
    std::memcpy(&handle, frameLocals + offset, sizeof(handle));
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size())
        return "null";
    if (m_slotKinds[static_cast<size_t>(handle)] == RTK_Boxed)
        return FormatDebugBoxed(
            m_structHeap[static_cast<size_t>(handle)]);
    //Defensive: any other record shape falls back to the one-level heap
    //display rather than a bare handle number.
    return FormatDebugHeapValue(handle);
}

} // namespace nlang
