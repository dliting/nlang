/*---
IntrinsicsString.cpp — built-in string method intrinsics.

Two families live here:
  ids 42/43   String.Equals / String.GetHashCode (Phase 8e-1, migrated
              from VmExecutor.cpp unchanged — ids and semantics frozen)
  ids 95-106  the 12 Phase 11 Step 3 methods (kStringMethodTable in
              StdLib.h is the resolver/codegen counterpart)

ABI (mirror of string.equals): receiver string pool idx at callParamBase
slot 0, arguments from slot 1 upward.

Byte semantics (user decision #7, Go/Lua model): length/substring/
indexOf are byte offsets; UTF-8 byte order == code point order. Case
conversion is ASCII-only. Argument/parse errors raise the base
Exception; substring range errors raise IndexOutOfBoundsException.
---*/
#include "VmExecutor.h"
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace nlang
{

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

//Read one string operand (receiver at slot 0, args from slot 1). Returns
//by value: the pool can grow during an intrinsic (result strings), and a
//held reference would dangle. Out-of-range idx reads as "" — same
//defensive shape the migrated Equals/GetHashCode always had.
static std::string ReadStrArg(const std::vector<std::string>& pool,
    const uint8_t* locals, uint16_t callParamBase, int slot)
{
    int32_t idx;
    std::memcpy(&idx, locals + callParamBase + slot * VALUE_SIZE, sizeof(idx));
    if (idx < 0 || static_cast<size_t>(idx) >= pool.size())
        return "";
    return pool[static_cast<size_t>(idx)];
}

//Phase 11 Step 3: allocate one boxed-value heap slot (layout per OP_Box:
//slot[0]=typeTag, slot[1]=value bits, m_slotKinds=RTK_Boxed). Extracted
//from the OP_Box body so split (and later fs.listFiles) share the exact
//allocation semantics — always allocate, even for value 0 (null literals
//never reach boxing; treating 0 as null broke List<int>.add(0)).
int32_t VmExecutor::AllocBoxedValue(uint8_t typeTag, int32_t val)
{
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(2, 0);
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(2, 0);
        m_slotKinds.push_back(0);
        m_slotStructIdx.push_back(0);
    }
    m_structHeap[static_cast<size_t>(heapIdx)][0] =
        static_cast<int32_t>(typeTag);
    m_structHeap[static_cast<size_t>(heapIdx)][1] = val;
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Boxed;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    m_gcPending = true;
    return heapIdx;
}

bool VmExecutor::ExecuteIntrinsicString(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    //---- Phase 8e-1 protocol methods (migrated verbatim) ----
    case INTR_String_Equals:
    {
        std::string a = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string b = ReadStrArg(m_stringPool, locals, callParamBase, 1);
        int32_t result = (a == b) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_GetHashCode:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        int32_t hash = static_cast<int32_t>(
            std::hash<std::string>{}(s));
        std::memcpy(pResult, &hash, sizeof(hash));
        return true;
    }
    //---- Phase 11 Step 3 methods ----
    case INTR_String_Substring:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        int32_t start, end;
        std::memcpy(&start, locals + callParamBase + VALUE_SIZE,
            sizeof(start));
        std::memcpy(&end, locals + callParamBase + 2 * VALUE_SIZE,
            sizeof(end));
        //Both offsets always staged by codegen: the 1-arg form is
        //lowered with end = receiver.length() (STD_ReceiverLength in
        //kStringMethodTable), so no absent-argument sentinel exists.
        if (start < 0 || static_cast<size_t>(start) > s.size()
            || end < start || static_cast<size_t>(end) > s.size())
            RaiseNlangException(m_oobExcClassIdx,
                "string.substring: range [" + std::to_string(start) + ", "
                + std::to_string(end) + ") outside string of "
                + std::to_string(s.size()) + " bytes.");
        std::string out = s.substr(static_cast<size_t>(start),
            static_cast<size_t>(end - start));
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(out));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_String_IndexOf:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string needle = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        //Empty needle finds at offset 0 (std::string::find semantics).
        size_t hit = s.find(needle);
        int32_t result = (hit == std::string::npos)
            ? -1 : static_cast<int32_t>(hit);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_StartsWith:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string prefix = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        int32_t result = (s.size() >= prefix.size()
            && 0 == s.compare(0, prefix.size(), prefix)) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_EndsWith:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string suffix = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        int32_t result = (s.size() >= suffix.size()
            && 0 == s.compare(s.size() - suffix.size(), suffix.size(),
                suffix)) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_Contains:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string needle = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        int32_t result = s.find(needle) != std::string::npos ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_ToUpper:
    case INTR_String_ToLower:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        const bool toUpper = (intrinsicId == INTR_String_ToUpper);
        //ASCII only (decision #7): no locale, no UTF-8 case mapping —
        //non-ASCII bytes pass through unchanged.
        for (auto& ch : s)
        {
            if (toUpper && ch >= 'a' && ch <= 'z')
                ch = static_cast<char>(ch - 'a' + 'A');
            else if (!toUpper && ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        }
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(s));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_String_Trim:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        auto isWs = [](char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'
                || ch == '\f' || ch == '\v';
        };
        size_t b = 0, e = s.size();
        while (b < e && isWs(s[b])) ++b;
        while (e > b && isWs(s[e - 1])) --e;
        std::string out = s.substr(b, e - b);
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(out));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_String_Split:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string sep = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        if (sep.empty())
            RaiseNlangExceptionBase(
                "string.split: the separator must not be empty.");
        //Split into parts first (plain strings), then build the List so no
        //pool reference is held across allocation (trap: pool growth).
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true)
        {
            size_t hit = s.find(sep, pos);
            if (hit == std::string::npos)
            {
                parts.push_back(s.substr(pos));
                break;
            }
            parts.push_back(s.substr(pos, hit - pos));
            pos = hit + sep.size();
        }
        //List<string> = List class instance + handle (INTR_Dict_Keys
        //recipe); elements are boxed-string heap slots.
        if (m_listClassIdx < 0)
            throw std::runtime_error(
                "NLang VM: List class not registered");
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        int32_t listHandle = AllocListHandle();
        m_structHeap[static_cast<size_t>(listHeapIdx)]
            [kListHandleFieldOffset] = listHandle;
        auto& dst = m_listStore[listHandle - 1].elements;
        dst.reserve(parts.size());
        for (auto& part : parts)
        {
            int32_t poolIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(part));
            dst.push_back(AllocBoxedValue(RTK_String, poolIdx));
        }
        std::memcpy(pResult, &listHeapIdx, sizeof(listHeapIdx));
        return true;
    }
    case INTR_String_Replace:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        std::string oldStr = ReadStrArg(m_stringPool, locals,
            callParamBase, 1);
        std::string newStr = ReadStrArg(m_stringPool, locals,
            callParamBase, 2);
        if (oldStr.empty())
            RaiseNlangExceptionBase(
                "string.replace: the searched text must not be empty.");
        //Left-to-right, non-overlapping ("aaa".replace("aa","ba") == "baa").
        std::string out;
        out.reserve(s.size());
        size_t pos = 0;
        while (true)
        {
            size_t hit = s.find(oldStr, pos);
            if (hit == std::string::npos)
            {
                out.append(s, pos, s.size() - pos);
                break;
            }
            out.append(s, pos, hit - pos);
            out += newStr;
            pos = hit + oldStr.size();
        }
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(out));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_String_ToInt:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        //Strict whole-string parse: no leading whitespace (strtol would
        //skip it — Java-compatible strictness) and full consumption. The
        //int32 bounds rationale lives at the check below.
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            RaiseNlangExceptionBase(
                "string.toInt: \"" + s + "\" is not a valid int.");
        errno = 0;
        char* end = nullptr;
        long parsed = std::strtol(s.c_str(), &end, 10);
        //Explicit int32 bounds: `long` is 32-bit on the supported
        //platforms (ERANGE catches overflow there), but an LP64 host
        //parses "5000000000" cleanly and the cast would truncate.
        if (end == s.c_str() || *end != '\0' || errno == ERANGE
            || parsed < INT32_MIN || parsed > INT32_MAX)
            RaiseNlangExceptionBase(
                "string.toInt: \"" + s + "\" is not a valid int.");
        int32_t result = static_cast<int32_t>(parsed);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_ToFloat:
    {
        std::string s = ReadStrArg(m_stringPool, locals, callParamBase, 0);
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            RaiseNlangExceptionBase(
                "string.toFloat: \"" + s + "\" is not a valid float.");
        errno = 0;
        char* end = nullptr;
        float parsed = std::strtof(s.c_str(), &end);
        //isfinite rejects inf/NaN results, including the ERANGE overflow
        //value HUGE_VALF (underflow to 0 is fine and stays accepted).
        if (end == s.c_str() || *end != '\0' || !std::isfinite(parsed))
            RaiseNlangExceptionBase(
                "string.toFloat: \"" + s + "\" is not a valid float.");
        std::memcpy(pResult, &parsed, sizeof(parsed));
        return true;
    }
    default:
        return false;
    }
}

} //namespace nlang
