// --- VmExecutor debug view + value formatters (ndb support) ---
// IVmDebugView implementation (frozen-time read-only queries) and the
// shallow formatters backing it. Separate TU by design: VmExecutor.cpp
// (the execution core) only ever gains checkpoint calls; every line of
// inspection logic lives here. Formatters are const and MUST NOT
// execute NLang code (no toString dispatch) or allocate on the NLang
// heap — the heap is frozen and consistent at checkpoint time.

#include "VmExecutor.h"
#include "IDebugHooks.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nlang {

namespace {

//Cap for one-level field/element dumps ("<Point{x=1, y=2, ...}>").
const int kMaxElementsShown = 5;

const char* DebugKindName(uint8_t kind) {
    switch (kind) {
    case RTK_Int32:  return "int";
    case RTK_Float:  return "float";
    case RTK_String: return "string";
    case RTK_Struct: return "struct";
    case RTK_Class:  return "class";
    case RTK_Array:  return "array";
    case RTK_Boxed:  return "boxed";
    case RTK_Func:   return "func";
    default:         return "unknown";
    }
}

std::string FormatFloatBits(int32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof(f));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", f);
    return buf;
}

} // namespace

uint16_t VmExecutor::CurrentFuncIdx() const {
    if (m_callStack.empty() || !m_currModule) return 0;
    return static_cast<uint16_t>(
        m_callStack.back().func - m_currModule->functions.data());
}

void VmExecutor::FireOnThrow() {
    if (!m_pDebugHooks) return;
    DebugStopInfo stop;
    if (!m_callStack.empty()) {
        stop.pc = m_callStack.back().currentPc;
        stop.line = m_callStack.back().currentLine;
    }
    stop.funcIdx = CurrentFuncIdx();
    stop.depth = m_callStack.size();
    m_pDebugHooks->OnThrow(stop, *this);
}

// --- IVmDebugView ---

size_t VmExecutor::FrameCount() const {
    return m_callStack.size();
}

DebugFrameInfo VmExecutor::FrameInfo(size_t depth) const {
    DebugFrameInfo info;
    if (depth >= m_callStack.size()) return info;
    const auto& frame = m_callStack[m_callStack.size() - 1 - depth];
    if (frame.func) {
        info.funcName = frame.func->name;
        info.sourceFile = frame.func->sourceFile;
        //funcIdx for the ndb `x` command (disassembly needs the
        //CompiledFunction; bare names can collide across classes).
        if (m_currModule)
            info.funcIdx = static_cast<uint16_t>(
                frame.func - m_currModule->functions.data());
    } else {
        info.funcName = "<unknown>";
    }
    info.line = frame.currentLine;
    info.pc = frame.currentPc;
    return info;
}

std::vector<DebugLocalValue> VmExecutor::FrameLocals(size_t depth) const {
    std::vector<DebugLocalValue> out;
    if (depth >= m_callStack.size()) return out;
    const auto& frame = m_callStack[m_callStack.size() - 1 - depth];
    if (!frame.func) return out;
    for (const auto& ld : frame.func->locals) {
        DebugLocalValue v;
        v.name = ld.name;
        v.kindName = DebugKindName(ld.typeKind);
        v.display = FormatDebugLocalSlot(ld, frame.locals);
        out.push_back(std::move(v));
    }
    return out;
}

// --- formatters ---

std::string VmExecutor::FormatDebugLocalSlot(const LocalDescriptor& ld,
    const uint8_t* frameLocals) const {
    int32_t raw = 0;
    std::memcpy(&raw, frameLocals + ld.offset, sizeof(raw));
    switch (ld.typeKind) {
    case RTK_Int32:  return std::to_string(raw);
    case RTK_Float:  return FormatFloatBits(raw);
    case RTK_String: return FormatDebugStringIdx(raw);
    case RTK_Struct:
    case RTK_Class:
    case RTK_Array:
    case RTK_Func:
        return FormatDebugHeapValue(raw);
    default:
        return "<unknown>";
    }
}

//String locals hold a string-pool idx (no null sentinel; out-of-range
//mirrors FormatArray's guard).
std::string VmExecutor::FormatDebugStringIdx(int32_t idx) const {
    if (idx >= 0 && static_cast<size_t>(idx) < m_stringPool.size())
        return QuoteString(m_stringPool[static_cast<size_t>(idx)]);
    return "\"\"";
}

std::string VmExecutor::FormatDebugBoxed(int32_t tag, int32_t val) const {
    if (tag == RTK_Int32) return std::to_string(val);
    if (tag == RTK_Float) return FormatFloatBits(val);
    if (tag == RTK_String) return FormatDebugStringIdx(val);
    return "<unknown>";
}

//Entry point for a reference-typed slot: renders ONE level (fields or
//elements), nested references as short tags.
std::string VmExecutor::FormatDebugHeapValue(int32_t heapIdx) const {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "null";
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    switch (m_slotKinds[static_cast<size_t>(heapIdx)]) {
    case RTK_Boxed:
        return FormatDebugBoxed(slot[0], slot[1]);
    case RTK_Class: {
        int32_t classIdx = slot[0];
        if (classIdx == m_listClassIdx)
            return FormatDebugList(slot[kListHandleFieldOffset]);
        if (classIdx == m_dictClassIdx)
            return FormatDebugDict(slot[kListHandleFieldOffset]);
        return FormatDebugClassInstance(heapIdx);
    }
    case RTK_Struct:
        return FormatDebugStructInstance(heapIdx);
    case RTK_Array:
        return FormatDebugArray(heapIdx);
    case RTK_Func:
        return "Func";
    default:
        return "<unknown>";
    }
}

std::string VmExecutor::FormatDebugClassInstance(int32_t heapIdx) const {
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    int32_t classIdx = slot[0];
    if (!m_currModule
        || classIdx < 0
        || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        return "<object>";
    const auto& cc = m_currModule->classes[static_cast<size_t>(classIdx)];
    std::string result = cc.name + "{";
    int shown = 0;
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (shown == kMaxElementsShown) { result += ", ..."; break; }
        if (shown > 0) result += ", ";
        result += cc.fieldNames[i] + "="
            + FormatDebugField(slot[1 + i], cc.fieldTypeKinds[i]);
        ++shown;
    }
    result += "}";
    return result;
}

std::string VmExecutor::FormatDebugStructInstance(int32_t heapIdx) const {
    //Struct identity lives in m_slotStructIdx (same slot as array's
    //arrayTypeIdx) — struct locals are heap idxs, never frame-inline.
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(heapIdx)];
    if (!m_currModule
        || structIdx >= m_currModule->structs.size())
        return "<struct>";
    const auto& cs = m_currModule->structs[structIdx];
    std::string result = cs.name + "{";
    int shown = 0;
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (shown == kMaxElementsShown) { result += ", ..."; break; }
        if (shown > 0) result += ", ";
        result += cs.fieldNames[i] + "="
            + FormatDebugField(slot[i], cs.fieldTypeKinds[i]);
        ++shown;
    }
    result += "}";
    return result;
}

//One field/element cell by declared kind. Dual discriminators (mirror
//MarkPhase): declared kind prunes primitives; array fields carry the
//declaration-side RTK_Array in .nmod (array redesign B) and render via
//the array formatter. Only Class/Struct/Func declared kinds fall
//through to the runtime slotKind — primitives early-return above, so
//no int value can reach the ref-tag path.
std::string VmExecutor::FormatDebugField(int32_t raw,
    uint16_t declaredKind) const {
    switch (declaredKind) {
    case RTK_Int32:  return std::to_string(raw);
    case RTK_Float:  return FormatFloatBits(raw);
    case RTK_String: return FormatDebugStringIdx(raw);
    case RTK_Array:
        if (raw > 0 && static_cast<size_t>(raw) < m_slotKinds.size())
            return FormatDebugArray(raw);
        return "null";
    default: break;
    }
    if (raw <= 0 || static_cast<size_t>(raw) >= m_slotKinds.size())
        return "null";
    uint8_t runtime = m_slotKinds[static_cast<size_t>(raw)];
    if (runtime == RTK_Boxed) {
        const auto& slot = m_structHeap[static_cast<size_t>(raw)];
        return FormatDebugBoxed(slot[0], slot[1]);
    }
    return FormatDebugRefShort(raw);
}

//List/Dict elements (and map keys/values) are uniformly heap idxs.
std::string VmExecutor::FormatDebugElementHeap(int32_t heapIdx) const {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "null";
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_Boxed) {
        const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
        return FormatDebugBoxed(slot[0], slot[1]);
    }
    return FormatDebugRefShort(heapIdx);
}

//Short tag for a nested reference: `<Point>` / `<int[3]>` / `Func`.
std::string VmExecutor::FormatDebugRefShort(int32_t heapIdx) const {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "null";
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    switch (m_slotKinds[static_cast<size_t>(heapIdx)]) {
    case RTK_Class: {
        int32_t classIdx = slot[0];
        if (classIdx == m_listClassIdx) return "List";
        if (classIdx == m_dictClassIdx) return "Dict";
        if (m_currModule
            && classIdx >= 0
            && static_cast<size_t>(classIdx) < m_currModule->classes.size())
            return "<" + m_currModule->classes[
                static_cast<size_t>(classIdx)].name + ">";
        return "<object>";
    }
    case RTK_Struct: {
        uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(heapIdx)];
        if (m_currModule
            && structIdx < m_currModule->structs.size())
            return "<" + m_currModule->structs[structIdx].name + ">";
        return "<struct>";
    }
    case RTK_Array: {
        uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
        int32_t length = slot[2];
        uint8_t elemKind = RTK_Int32;
        if (m_currModule
            && arrayTypeIdx < m_currModule->arrayTypes.size())
            elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
        return std::string("<") + DebugKindName(elemKind) + "["
            + std::to_string(length) + "]>";
    }
    case RTK_Boxed:
        return FormatDebugBoxed(slot[0], slot[1]);
    case RTK_Func:
        return "Func";
    default:
        return "<unknown>";
    }
}

std::string VmExecutor::FormatDebugArray(int32_t heapIdx) const {
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    int32_t length = slot[2];
    uint8_t elemKind = RTK_Int32;
    if (m_currModule
        && arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    std::string result = std::string(DebugKindName(elemKind)) + "["
        + std::to_string(length) + "]{";
    int bound = length < kMaxElementsShown ? length : kMaxElementsShown;
    for (int32_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugField(
            slot[3 + static_cast<size_t>(i)], elemKind);
    }
    if (length > bound) result += ", ...";
    result += "}";
    return result;
}

std::string VmExecutor::FormatDebugList(int32_t handle) const {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_listStore.size())
        return "List[0]{}";
    const auto& elems =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
    std::string result = "List[" + std::to_string(elems.size()) + "]{";
    size_t bound = elems.size() < static_cast<size_t>(kMaxElementsShown)
        ? elems.size() : static_cast<size_t>(kMaxElementsShown);
    for (size_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugElementHeap(elems[i]);
    }
    if (elems.size() > bound) result += ", ...";
    result += "}";
    return result;
}

std::string VmExecutor::FormatDebugDict(int32_t handle) const {
    if (handle <= 0
        || static_cast<size_t>(handle) > m_dictStore.size())
        return "Dict[0]{}";
    const auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
    std::string result = "Dict[" + std::to_string(entries.size()) + "]{";
    size_t bound = entries.size() < static_cast<size_t>(kMaxElementsShown)
        ? entries.size() : static_cast<size_t>(kMaxElementsShown);
    for (size_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugElementHeap(entries[i].first) + ": "
            + FormatDebugElementHeap(entries[i].second);
    }
    if (entries.size() > bound) result += ", ...";
    result += "}";
    return result;
}

} // namespace nlang
