/*---
    VmExecutorFormat.cpp — toString 格式化机器（QuoteString/Format 族/虚调用渲染）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
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
        int32_t tag = slot[0];
        int32_t val = slot[1];
        if (tag == RTK_Int32) {
            return std::to_string(val);
        } else if (tag == RTK_Float) {
            float fv;
            std::memcpy(&fv, &val, sizeof(fv));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fv);
            return buf;
        } else if (tag == RTK_String) {
            return QuoteString(StrVal(val));
        }
        return "<unknown>";
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
    for (int32_t i = 0; i < length; ++i) {
        if (i > 0) result += ", ";
        int32_t elemVal = slot[3 + static_cast<size_t>(i)];
        switch (elemKind) {
        case RTK_Int32:
            result += std::to_string(elemVal);
            break;
        case RTK_Float: {
            float fv;
            std::memcpy(&fv, &elemVal, sizeof(fv));
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", fv);
            result += buf;
            break;
        }
        case RTK_String:
            result += QuoteString(StrVal(elemVal));
            break;
        case RTK_Struct:
            result += "<struct>";
            break;
        case RTK_Class:
        case RTK_Boxed:
        case RTK_Array:
            result += FormatHeapValue(elemVal, depth);
            break;
        default:
            result += "<unknown>";
            break;
        }
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
    int funcIndex = -1;
    int searchClassIdx = classIdx;
    while (searchClassIdx >= 0
        && searchClassIdx < static_cast<int>(m_currModule->classes.size())) {
        const auto& cc =
            m_currModule->classes[static_cast<size_t>(searchClassIdx)];
        for (uint16_t idx : cc.methodIndices) {
            if (idx < m_currModule->functions.size()
                && m_currModule->functions[idx].name == "toString") {
                funcIndex = static_cast<int>(idx);
                break;
            }
        }
        if (funcIndex >= 0) break;
        searchClassIdx = cc.superClassIdx;
    }
    if (funcIndex < 0)
        return std::string("<") +
            m_currModule->classes[static_cast<size_t>(classIdx)].name + ">";
    const CompiledFunction& callee =
        m_currModule->functions[static_cast<size_t>(funcIndex)];
    //Synthetic 4-byte locals frame: just thisHeapIdx at offset 0.
    alignas(int32_t) uint8_t paramFrame[4] = {0};
    std::memcpy(paramFrame, &thisHeapIdx, sizeof(thisHeapIdx));
    alignas(int32_t) uint8_t resultBuf[4] = {0};
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
            + m_currModule->classes[static_cast<size_t>(classIdx)].name);
    } else {
        std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
        uint16_t paramBytes = callee.paramCount * sizeof(int32_t);
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

} // namespace nlang
