/*---
    VmExecutorFormat.cpp — toString 格式化机器（QuoteString/Format 族/虚调用渲染）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/RnTypes.h>
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

//Phase 9b-pre: collection toString helpers.
//Python-style formatting: [a, b, c] / {k: v}. Strings quoted with repr-style
//escape. Cyclic/nested structures bounded by TOSTRING_DEPTH_LIMIT (64).
//Throws std::runtime_error on depth overflow; caught by main()'s catch and
//surfaced as exit(1) like other VM errors.

std::string VmExecutor::QuoteString(const std::string& s) const {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
    return out;
}

//Phase 13: shared renderer for function handles. slot[0] holds a function
//index for static handles (form 0) or a string-constant index for
//virtual-dispatch handles (form 1, Step 2).
std::string VmExecutor::FormatFuncHandle(int32_t heapIdx) const {
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    if (slot[2] == kFuncFormStatic) {
        uint16_t funcIdx = static_cast<uint16_t>(slot[0]);
        if (funcIdx < m_currModule->functions.size())
            return "func " + m_currModule->functions[funcIdx].name;
        return "func <invalid>";
    }
    int32_t nameIdx = slot[0];
    //slot[0] of a virtual-dispatch handle stays a compile-time constant
    //index (D2 scheme b) — read the module's constant table, not the
    //runtime string store.
    if (nameIdx >= 0
        && static_cast<size_t>(nameIdx) < m_currModule->stringConstants.size())
        return "method "
            + m_currModule->stringConstants[static_cast<size_t>(nameIdx)];
    return "method <invalid>";
}

void VmExecutor::OpFunc_to_str(uint8_t* pResult) {
    //K8: lock-free — benign heap reads plus a self-locking mint.
    //Accumulator-shaped like OP_Array_to_str: the handle comes in
    //via pResult, the interned string index goes back out through
    //pResult. Direct conversion paths bypass the member-dispatch
    //NPE site, so guard the null sentinel here.
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    std::string s;
    if (heapIdx <= 0) {
        s = "<null>";
    } else if (static_cast<size_t>(heapIdx) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Func) {
        throw std::runtime_error(
            "NLang VM: func_to_str on stale or non-function value");
    } else {
        s = FormatFuncHandle(heapIdx);
    }
    int32_t handle = MintNewString(std::move(s));
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpFuncEquality(BytecodeReader& reader, uint8_t* locals, OpCode op) {
    //K8: lock-free — heap size, slot kinds and record reads are all
    //benign under the sharing model (chunked arrays, no slot motion).
    bool bNeg = (op == OpCode::OP_Ne_func);
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t idxA, idxB;
    std::memcpy(&idxA, locals + lhs, sizeof(idxA));
    std::memcpy(&idxB, locals + rhs, sizeof(idxB));
    //Heap index 0 is the null sentinel - reading its slots is UB,
    //so null participates as plain index comparison.
    bool bEqual;
    if (idxA <= 0 || idxB <= 0) {
        bEqual = (idxA == idxB);
    } else if (static_cast<size_t>(idxA) >= m_structHeap.size()
        || static_cast<size_t>(idxB) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(idxA)] != RTK_Func
        || m_slotKinds[static_cast<size_t>(idxB)] != RTK_Func) {
        throw std::runtime_error(
            "NLang VM: function equality on a stale non-function value");
    } else {
        //The container matcher's comparator is the same triple rule
        //(target, receiver, form) — share it instead of re-spelling it.
        bEqual = FuncRecordValuesEqual(idxA, idxB);
    }
    int32_t r = ((bEqual != bNeg) ? 1 : 0);
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpStructEquality(BytecodeReader& reader, uint8_t* locals, OpCode op) {
    //v0.13.0 D2: struct ==/!= — field-value equality through the same
    //comparator the container entries use. Read-only, so the K8
    //benign-sharing note of OpFuncEquality applies unchanged.
    bool bNeg = (op == OpCode::OP_Ne_struct);
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t idxA, idxB;
    std::memcpy(&idxA, locals + lhs, sizeof(idxA));
    std::memcpy(&idxB, locals + rhs, sizeof(idxB));
    //Null sentinel 0 participates as plain index comparison (OpFuncEquality
    //rule); the resolver gate only emits this opcode for same-type struct
    //operands, so a stale non-struct read is a broken image, not a user path.
    bool bEqual;
    if (idxA <= 0 || idxB <= 0) {
        bEqual = (idxA == idxB);
    } else if (static_cast<size_t>(idxA) >= m_structHeap.size()
        || static_cast<size_t>(idxB) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(idxA)] != RTK_Struct
        || m_slotKinds[static_cast<size_t>(idxB)] != RTK_Struct) {
        throw std::runtime_error(
            "NLang VM: struct equality on a stale non-struct value");
    } else {
        bEqual = StructValuesEqual(idxA, idxB);
    }
    int32_t r = ((bEqual != bNeg) ? 1 : 0);
    std::memcpy(locals + lhs, &r, sizeof(r));
}

std::string VmExecutor::FormatHeapValue(int32_t heapIdx, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return "<null>";
    uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    switch (kind) {
    case RTK_Boxed: {
        //0.7.5: boxed records are {tag, lo, hi} — the whole scalar
        //family renders through the canonical registry renderer; only
        //the string tag keeps its own (quoting) arm.
        if (slot[0] == RTK_String)
            return QuoteString(StrVal(slot[1]));
        return FormatScalarValue(static_cast<uint8_t>(slot[0]),
            slot[1], slot[2]);
    }
    case RTK_Class: {
        int32_t classIdx = slot[0];
        if (classIdx == m_listClassIdx) {
            int32_t handle = slot[kListHandleFieldOffset];
            return FormatList(handle, depth + 1);
        }
        if (classIdx == m_dictClassIdx) {
            int32_t handle = slot[kListHandleFieldOffset];
            return FormatDict(handle, depth + 1);
        }
        return InvokeVirtualToString(heapIdx);
    }
    case RTK_Struct:
        return "<struct>";
    case RTK_Array:
    case RTK_ArrayView:
        //FormatArray branches to the view walk on the record's own kind
        //(Task 10) — the bracket output is the view's logical content.
        return FormatArray(heapIdx, depth + 1);
    case RTK_Func:
        //Fifth FormatHeapValue consumer (container formatting); the four
        //direct conversion paths go through OP_Func_to_str instead.
        return FormatFuncHandle(heapIdx);
    default:
        return "<unknown>";
    }
}

//0.7.5: the family's single scalar format decision. Carriers read
//their own width from the little-endian staging buffer (narrow rows
//find the value-extended low bytes; 8-byte rows read lo|hi combined),
//so uint renders unsigned and long/ulong/double render their full
//width — whatever the registry's per-category ValueToString decides.
std::string VmExecutor::FormatScalarValue(uint8_t rtk, int32_t lo,
    int32_t hi) const {
    int i = ScalarPrimIndexOfRtk(rtk);
    if (i < 0)
        return "<unknown>";
    //Registry singletons intern names through IdString — the host has
    //run Runtime::StaticInit() long before any formatting (same
    //contract as OP_Prim_to_str).
    uint8_t value[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(value, &lo, sizeof(lo));
    std::memcpy(value + 4, &hi, sizeof(hi));
    return RnBuiltinDataType::InstanceOf(kScalarPrims[i].kind)
        ->ValueToString(value);
}

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
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
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

//v0.12.7 Task 5: shared array formatting — null/stale/kind validation,
//FormatArray, mint. OP_Array_to_str and the Array.toString intrinsic
//format through this one path (K8: the walker snapshots container
//structure under the Container domain lock internally; the mint
//self-locks).
int32_t VmExecutor::MintArrayString(int32_t heapIdx) {
    std::string s;
    if (heapIdx <= 0) {
        s = "<null>";
    } else if (static_cast<size_t>(heapIdx) >= m_structHeap.size()) {
        throw std::runtime_error(
            "NLang VM: array_to_str on stale reference");
    } else if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array
               && m_slotKinds[static_cast<size_t>(heapIdx)]
                      != RTK_ArrayView) {
        throw std::runtime_error(
            "NLang VM: array_to_str on non-array");
    } else {
        s = FormatArray(heapIdx, 0);
    }
    return MintNewString(std::move(s));
}

std::string VmExecutor::FormatList(int32_t handle, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (handle <= 0 || static_cast<size_t>(handle) > m_listStore.size())
        return "[]";
    //K8: snapshot under Container, format outside it (user overrides
    //re-enter domains freely).
    std::vector<int32_t> elems = SnapshotListElements(handle);
    if (elems.empty()) return "[]";
    std::string result = "[";
    for (size_t i = 0; i < elems.size(); ++i) {
        if (i > 0) result += ", ";
        result += FormatHeapValue(elems[i], depth);
    }
    result += "]";
    return result;
}

std::string VmExecutor::FormatDict(int32_t handle, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (handle <= 0 || static_cast<size_t>(handle) > m_dictStore.size())
        return "{}";
    //K8: snapshot under Container, format outside it (see FormatList).
    std::vector<std::pair<int32_t, int32_t>> entries
        = SnapshotDictEntries(handle);
    if (entries.empty()) return "{}";
    std::string result = "{";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) result += ", ";
        result += FormatHeapValue(entries[i].first, depth);
        result += ": ";
        result += FormatHeapValue(entries[i].second, depth);
    }
    result += "}";
    return result;
}

std::string VmExecutor::CallToStringOverride(const CompiledFunction& callee,
    int32_t thisHeapIdx, int32_t classIdx) {
    //Synthetic one-slot locals frame: just thisHeapIdx at offset 0.
    //Both buffers must be full uniform frame cells — the override's
    //final OP_VarLocal return copy and the param-block memcpy both
    //move kFrameSlotBytes.
    alignas(int64_t) uint8_t paramFrame[kFrameSlotBytes] = {0};
    std::memcpy(paramFrame, &thisHeapIdx, sizeof(thisHeapIdx));
    alignas(int64_t) uint8_t resultBuf[kFrameSlotBytes] = {0};
    if (callee.intrinsicId != INTR_None) {
        ExecuteIntrinsic(callee.intrinsicId, 0, paramFrame, resultBuf);
    } else if (callee.isNative) {
        //Never run a native record through the dispatch loop: its bytecode
        //is empty, so the loop falls straight through and the untouched
        //(zeroed) result buffer fabricates a string. The by-name native
        //table cannot express a per-class toString and a NativeFn cannot
        //intern a string anyway — refuse until 9f-2 marshalling exists.
        throw std::runtime_error(
            "NLang VM: native toString cannot serve implicit formatting: "
            + LeafNameOfKey(
                m_currModule->classes[static_cast<size_t>(classIdx)].name));
    } else {
        //Nested window: push onto the shared frame stack and run until
        //the depth captured at entry (GC keeps seeing the nested frames
        //through the enclosing participation root; an unhandled throw
        //unwinds only this window before propagating to the caller's
        //frame — exactly the recursive engine's shape).
        const size_t baseDepth = EvalStack().size();
        //deferSafepoint: the receiver exists nowhere else during staging.
        uint8_t* staged = PushRootFrame(callee, resultBuf, /*deferSafepoint=*/true);
        uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
        if (paramBytes > 0 && paramBytes <= callee.localsSize)
            std::memcpy(staged, paramFrame, paramBytes);
        CheckGCSafepoint();
        //Nested evaluation window (P2-4): suspension parks through the
        //pump — other contexts run on this thread, the window resumes
        //in place, and the C++ frames between here and the outer opcode
        //never unwind. The guard lives inside RunNestedWindow now (it
        //covers only the fast-path frames).
        RunNestedWindow(baseDepth);
    }
    int32_t strIdx;
    std::memcpy(&strIdx, resultBuf, sizeof(strIdx));
    //Execution path: StrVal flattens a cons result in place on first
    //read; the all-StrValCopy era re-copied the whole unflattened chain
    //on every toString call.
    return StrVal(strIdx);
}

std::string VmExecutor::InvokeVirtualToString(int32_t thisHeapIdx) {
    //Mirror OP_CallMethod's vtable walk: search class hierarchy for
    //a method named "toString". If found, call it; if the resolved
    //function is an intrinsic (Object.toString default or user override
    //on List/Dict), dispatch via ExecuteIntrinsic. If not found, fall
    //back to "<ClassName>" placeholder (shouldn't happen — every class
    //inherits Object.toString).
    if (thisHeapIdx <= 0
        || static_cast<size_t>(thisHeapIdx) >= m_structHeap.size())
        return "<null>";
    int32_t classIdx = ReceiverClassIndex(thisHeapIdx);
    if (classIdx < 0
        || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        return "<unknown>";
    //Same hierarchy walk as OP_CallMethod (FindMethodByName shares it);
    //the "toString" lookup below replaced a local copy of the loop.
    int funcIndex = FindMethodByName(classIdx, "toString");
    if (funcIndex < 0)
        return std::string("<") + LeafNameOfKey(
            m_currModule->classes[static_cast<size_t>(classIdx)].name)
            + ">";
    return CallToStringOverride(
        m_currModule->functions[static_cast<size_t>(funcIndex)],
        thisHeapIdx, classIdx);
}

} // namespace nlang
