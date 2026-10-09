// --- VmExecutor debug view + value formatters (ndb support) ---
// IVmDebugView implementation (frozen-time read-only queries) and the
// shallow formatters backing it. Separate TU by design: VmExecutor.cpp
// (the execution core) only ever gains checkpoint calls; every line of
// inspection logic lives here. Formatters are const and MUST NOT
// execute NLang code (no toString dispatch) or allocate on the NLang
// heap — the heap is frozen and consistent at checkpoint time.

#include "VmExecutor.h"
#include "IDebugHooks.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nlang {

namespace {

//Cap for one-level field/element dumps ("<Point{x=1, y=2, ...}>").
const int kMaxElementsShown = 5;

const char* DebugKindName(uint8_t kind) {
    //0.7.5: scalars name themselves through the registry (bool today,
    //the P4-P6 family later) — no per-kind arm to forget.
    int i = ScalarPrimIndexOfRtk(kind);
    if (i >= 0)
        return kScalarPrims[i].name;
    switch (kind) {
    case RTK_String: return "string";
    case RTK_Struct: return "struct";
    case RTK_Class:  return "class";
    case RTK_Array:  return "array";
    case RTK_Boxed:  return "boxed";
    case RTK_Func:   return "func";
    default:         return "unknown";
    }
}

//0.7.5 Task 11: debugger-only char rendering. The registry renderer
//gives toString semantics (the bare UTF-8 code point — right for
//printing and string building); a debugger local wants the value
//self-describing, so it carries an identity tag: the quoted code point
//plus the U+ hex form ('中' (U+4E2D)). Control characters and
//out-of-scalar values drop the quotes (no raw newlines in the display)
//but keep the tag.
std::string DebugCharDisplay(uint32_t cp) {
    const bool printable =
        (cp >= 0x20 && cp < 0x7F)
        || (cp >= 0xA0 && cp < 0xD800)
        || (cp >= 0xE000 && cp <= 0x10FFFF);
    char tag[16];
    std::snprintf(tag, sizeof(tag), "(U+%04llX)",
                  static_cast<unsigned long long>(cp));
    if (!printable)
        return tag;
    return "'" + Utf8EncodeCodePoint(cp) + "' " + tag;
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
        //D13: the table key IS the display name — free functions show as
        //"<package>.<name>" since phase 5, methods keep bare names.
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
        //Scope visibility: the frame is flat (slots exist from entry),
        //but a local joins the display only once the paused statement
        //PC reaches its declaration — declPc equals the anchor of the
        //declaring statement, and currentPc is the last executed
        //anchor, so pausing ON the decl line still shows the local with
        //its zero value (gdb/VS convention). The zero slot itself is
        //meaningful and must stay allocated: the GC root scan walks the
        //whole table from function entry. Same-name redeclarations
        //reuse the first declaration's position (over-approximation:
        //strictly fewer false leaks than no filter).
        if (ld.declPc > frame.currentPc)
            continue;
        DebugLocalValue v;
        v.name = ld.name;
        v.kindName = DebugKindName(ld.typeKind);
        v.display = FormatDebugLocalSlot(ld, frame.locals);
        out.push_back(std::move(v));
    }
    return out;
}

// --- formatters ---

//Frame slots are uniform kFrameSlotBytes cells: the low 4 bytes carry
//every narrow kind (value extension), 8-byte kinds fill the whole
//slot. Scalar rendering delegates to the canonical registry renderer;
//only string and reference locals keep their own arms.
std::string VmExecutor::FormatDebugLocalSlot(const LocalDescriptor& ld,
    const uint8_t* frameLocals) const {
    int32_t lo = 0, hi = 0;
    std::memcpy(&lo, frameLocals + ld.offset, sizeof(lo));
    std::memcpy(&hi, frameLocals + ld.offset + sizeof(hi), sizeof(hi));
    switch (ld.typeKind) {
    case RTK_String:
        return FormatDebugStringIdx(lo);
    case RTK_Char:
        //Locals get the debugger identity tag, not the toString form —
        //char elements inside arrays/boxes keep the plain renderer.
        return DebugCharDisplay(static_cast<uint32_t>(lo));
    case RTK_Struct:
    case RTK_Class:
    case RTK_Array:
    case RTK_Func:
        return FormatDebugHeapValue(lo);
    default:
        if (ScalarPrimIndexOfRtk(ld.typeKind) >= 0)
            return FormatScalarValue(ld.typeKind, lo, hi);
        return "<unknown>";
    }
}

//String locals hold a string-object handle (0 = null). StrValCopy is the
//non-mutating accessor — const formatters must not flatten in place —
//and reads null/invalid handles as "", rendered as "".
std::string VmExecutor::FormatDebugStringIdx(int32_t idx) const {
    return QuoteString(StrValCopy(idx));
}

//Boxed payload cells [1, 2] carry the value at the tag's registry
//width — the tag doubles as the declared kind, so the stride-aware
//field renderer reads it directly (0.7.5: boxed long/ulong render the
//combined 8 bytes instead of the old "<unknown>").
std::string VmExecutor::FormatDebugBoxed(const std::vector<int32_t>& slot) const {
    return FormatDebugField(slot, 1, static_cast<uint16_t>(slot[0]));
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
        return FormatDebugBoxed(slot);
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
    //Leaf name: the table key is package-qualified linkage identity; the
    //debugger value display names the type as source wrote it.
    std::string result = LeafNameOfKey(cc.name) + "{";
    int shown = 0;
    for (uint16_t i = 0; i < cc.fieldCount; ++i) {
        if (shown == kMaxElementsShown) { result += ", ..."; break; }
        if (shown > 0) result += ", ";
        result += cc.fieldNames[i] + "="
            + FormatDebugField(slot, 1 + static_cast<size_t>(i) * 2,
                cc.fieldTypeKinds[i]);
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
    std::string result = LeafNameOfKey(cs.name) + "{";
    int shown = 0;
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        if (shown == kMaxElementsShown) { result += ", ..."; break; }
        if (shown > 0) result += ", ";
        result += cs.fieldNames[i] + "="
            + FormatDebugField(slot, static_cast<size_t>(i) * 2,
                cs.fieldTypeKinds[i]);
        ++shown;
    }
    result += "}";
    return result;
}

//One field/element cell by declared kind, registry stride aware:
//8-byte scalar kinds (long/ulong; double with Task 7) read the cell
//pair [cellIdx, cellIdx+1], everything else the single cell. Scalars
//delegate to the canonical registry renderer. Dual discriminators
//(mirror MarkPhase): array fields carry the declaration-side RTK_Array
//in .ncu (array redesign B) and render via the array formatter; only
//Class/Struct/Func declared kinds fall through to the runtime
//slotKind — primitives early-return above, so no int value can reach
//the ref-tag path.
std::string VmExecutor::FormatDebugField(const std::vector<int32_t>& slot,
    size_t cellIdx, uint16_t declaredKind) const {
    if (declaredKind == RTK_String)
        return FormatDebugStringIdx(slot[cellIdx]);
    if (declaredKind == RTK_Array) {
        int32_t raw = slot[cellIdx];
        if (raw > 0 && static_cast<size_t>(raw) < m_slotKinds.size())
            return FormatDebugArray(raw);
        return "null";
    }
    if (ScalarPrimIndexOfRtk(static_cast<uint8_t>(declaredKind)) >= 0) {
        //Bounds-guard the hi cell: boxed records and well-formed field
        //tables always carry it; a malformed short slot reads 0.
        int32_t hi = cellIdx + 1 < slot.size() ? slot[cellIdx + 1] : 0;
        return FormatScalarValue(static_cast<uint8_t>(declaredKind),
            slot[cellIdx], hi);
    }
    int32_t raw = slot[cellIdx];
    if (raw <= 0 || static_cast<size_t>(raw) >= m_slotKinds.size())
        return "null";
    uint8_t runtime = m_slotKinds[static_cast<size_t>(raw)];
    if (runtime == RTK_Boxed) {
        return FormatDebugBoxed(m_structHeap[static_cast<size_t>(raw)]);
    }
    return FormatDebugRefShort(raw);
}

//List/Dict elements (and map keys/values) are uniformly heap idxs.
std::string VmExecutor::FormatDebugElementHeap(int32_t heapIdx) const {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "null";
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_Boxed)
        return FormatDebugBoxed(m_structHeap[static_cast<size_t>(heapIdx)]);
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
        if (classIdx == m_listClassIdx) return kBuiltinListTypeName;
        if (classIdx == m_dictClassIdx) return kBuiltinDictTypeName;
        if (m_currModule
            && classIdx >= 0
            && static_cast<size_t>(classIdx) < m_currModule->classes.size())
            return "<" + LeafNameOfKey(m_currModule->classes[
                static_cast<size_t>(classIdx)].name) + ">";
        return "<object>";
    }
    case RTK_Struct: {
        uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(heapIdx)];
        if (m_currModule
            && structIdx < m_currModule->structs.size())
            return "<" + LeafNameOfKey(
                m_currModule->structs[structIdx].name) + ">";
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
        return FormatDebugBoxed(slot);
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
    //0.7.5: registry stride — 8-byte scalar elements read the cell
    //pair through the same stride-aware field renderer as everyone
    //else; 1-cell kinds read their single cell.
    const int cells = ArrayElemCells(elemKind);
    for (int32_t i = 0; i < bound; ++i) {
        if (i > 0) result += ", ";
        result += FormatDebugField(slot, 3 + static_cast<size_t>(i) * cells,
            elemKind);
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
