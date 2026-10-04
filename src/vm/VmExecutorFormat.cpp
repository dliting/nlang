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
        const auto& a = m_structHeap[static_cast<size_t>(idxA)];
        const auto& b = m_structHeap[static_cast<size_t>(idxB)];
        bEqual = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2]);
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

std::string VmExecutor::FormatArray(int32_t heapIdx, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    int32_t length = slot[2];
    uint8_t elemKind = RTK_Int32;
    if (arrayTypeIdx < m_currModule->arrayTypes.size())
        elemKind = m_currModule->arrayTypes[arrayTypeIdx].elemKind;
    if (length <= 0) return "[]";
    std::string result = "[";
    const int cells = ArrayElemCells(elemKind);
    for (int32_t i = 0; i < length; ++i) {
        if (i > 0) result += ", ";
        FormatArrayElement(result, slot, 3 + static_cast<size_t>(i) * cells,
            elemKind, cells, depth);
    }
    result += "]";
    return result;
}

std::string VmExecutor::FormatList(int32_t handle, int depth) {
    if (depth > static_cast<int>(TOSTRING_DEPTH_LIMIT))
        throw std::runtime_error(
            "NLang VM: toString depth limit exceeded");
    if (handle <= 0 || static_cast<size_t>(handle) > m_listStore.size())
        return "[]";
    const auto& elems =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
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
    const auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
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
    //Both buffers must be full uniform frame cells — ExecuteFunction's
    //final OP_VarLocal return copy and the param-block memcpy both
    //move kFrameSlotBytes.
    alignas(int64_t) uint8_t paramFrame[kFrameSlotBytes] = {0};
    std::memcpy(paramFrame, &thisHeapIdx, sizeof(thisHeapIdx));
    alignas(int64_t) uint8_t resultBuf[kFrameSlotBytes] = {0};
    if (callee.intrinsicId != INTR_None) {
        ExecuteIntrinsic(callee.intrinsicId, 0, paramFrame, resultBuf);
    } else if (callee.isNative) {
        //Never run a native record through ExecuteFunction: its bytecode
        //is empty, so the loop falls straight through and the untouched
        //(zeroed) result buffer fabricates a string. The by-name native
        //table cannot express a per-class toString and a NativeFn cannot
        //intern a string anyway — refuse until 9f-2 marshalling exists.
        throw std::runtime_error(
            "NLang VM: native toString cannot serve implicit formatting: "
            + LeafNameOfKey(
                m_currModule->classes[static_cast<size_t>(classIdx)].name));
    } else {
        std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
        uint16_t paramBytes = callee.paramCount * kFrameSlotBytes;
        if (paramBytes > 0 && paramBytes <= callee.localsSize)
            std::memcpy(calleeLocals.data(), paramFrame, paramBytes);
        ExecuteFunction(callee, resultBuf, calleeLocals.data());
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
