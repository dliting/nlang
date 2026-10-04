/*---
IntrinsicsString.cpp — built-in string method intrinsics.

Two families live here:
  ids 42/43   String.Equals / String.GetHashCode (Phase 8e-1, migrated
              from VmExecutor.cpp unchanged — ids and semantics frozen)
  ids 95-112  the 18 table methods (Phase 11 Step 3 laid down 12; the
              0.7.5 char bridge appended the 6 code-point/parse methods
              — kStringMethodTable in StdLib.h is the resolver/codegen
              counterpart)

ABI (mirror of string.equals): receiver string handle at callParamBase
slot 0, arguments from slot 1 upward.

Byte semantics (user decision #7, Go/Lua model): length/substring/
indexOf are byte offsets; UTF-8 byte order == code point order. Case
conversion is ASCII-only. The 0.7.5 char bridge adds the code-point
surfaces on top (charAt decodes at a byte offset, charCount counts
code points); invalid UTF-8 raises like every other argument error.
Argument/parse errors raise the base Exception; substring range
errors raise IndexOutOfBoundsException.
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


//Phase 11 error model: argument/range errors raise the BASE Exception.
//Declared in VmExecutor.h; defined here because this is the one TU that
//still raises intrinsically since the math/io/fs families retired.
[[noreturn]] void VmExecutor::RaiseNlangExceptionBase(const std::string& msg)
{
    RaiseNlangException(m_exceptionClassIdx, msg);
}

//Read one string operand (receiver at slot 0, args from slot 1). Returns
//by value: the store can grow during an intrinsic (result strings), and a
//held reference would dangle. Null/out-of-range handles read as "" — same
//defensive shape the migrated Equals/GetHashCode always had.
static std::string ReadStrArg(VmExecutor& ex,
    const uint8_t* locals, uint16_t callParamBase, int slot)
{
    int32_t handle;
    std::memcpy(&handle, locals + callParamBase + slot * kFrameSlotBytes,
        sizeof(handle));
    return ex.StrValCopy(handle);   //args are consumed once; Copy avoids
                                    //any in-place flatten surprise mid-ABI
}

//Phase 11 Step 3: allocate one boxed-value heap slot (layout per OP_Box:
//cell[0]=typeTag, cell[1]=value low bits, cell[2]=value high bits,
//m_slotKinds=RTK_Boxed).
//Extracted from the OP_Box body so split share the exact allocation
//semantics — always allocate, even for value 0 (null literals never
//reach boxing; treating 0 as null broke List<int>.add(0)).
//0.7.5: payload width from the registry — 8-byte rows (long/ulong and
//later double) span cells [1..2]; every ≤4-byte tag (incl. the string
//and func handle families) rides cell[1] with the high cell zeroed by
//the assign/emplace above.
int32_t VmExecutor::AllocBoxedValue(uint8_t typeTag, int64_t val)
{
    int32_t heapIdx;
    if (!m_freeList.empty()) {
        heapIdx = m_freeList.back();
        m_freeList.pop_back();
        m_structHeap[static_cast<size_t>(heapIdx)].assign(3, 0);
    } else {
        heapIdx = static_cast<int32_t>(m_structHeap.size());
        m_structHeap.emplace_back(3, 0);
        m_slotKinds.push_back(0);
        m_slotStructIdx.push_back(0);
    }
    auto& rec = m_structHeap[static_cast<size_t>(heapIdx)];
    rec[0] = static_cast<int32_t>(typeTag);
    int pi = ScalarPrimIndexOfRtk(typeTag);
    if (pi >= 0 && kScalarPrims[pi].slotWidth == 8)
    {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(bits));
        rec[1] = static_cast<int32_t>(
            static_cast<uint32_t>(bits & 0xFFFFFFFFu));
        rec[2] = static_cast<int32_t>(
            static_cast<uint32_t>(bits >> 32));
    }
    else
    {
        rec[1] = static_cast<int32_t>(val);
    }
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Boxed;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = 0;
    m_gcPending = true;
    return heapIdx;
}

//0.7.5: decode the UTF-8 code point starting at byte i of s. Returns
//the code point (0..0x10FFFF) and stores its byte length in *len;
//returns -1 on any invalid shape (continuation lead, truncated or
//overlong form, surrogate, out of range). Shared by the charAt/
//charCount intrinsics; OP_StrForeachStep keeps its own inline decode
//(executor TU) — the raise wording stays per call site.
static int64_t DecodeCodePointAt(const std::string& s, size_t i, int& len)
{
    const auto byte = [&s](size_t k) {
        return static_cast<unsigned char>(s[k]);
    };
    const unsigned char lead = byte(i);
    int n = 0;                //continuation byte count
    int64_t cp = -1;
    if (lead < 0x80)                { n = 0; cp = lead; }
    else if ((lead & 0xE0) == 0xC0) { n = 1; cp = lead & 0x1F; }
    else if ((lead & 0xF0) == 0xE0) { n = 2; cp = lead & 0x0F; }
    else if ((lead & 0xF8) == 0xF0) { n = 3; cp = lead & 0x07; }
    else return -1;           //continuation byte or 0xF8+ lead
    const auto isCont = [&s](size_t k) {
        return k < s.size()
            && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80;
    };
    for (int k = 1; k <= n; ++k)
    {
        if (!isCont(i + static_cast<size_t>(k)))
            return -1;
        cp = (cp << 6) | (byte(i + static_cast<size_t>(k)) & 0x3F);
    }
    //Minimal-form (overlong), surrogate and range validation.
    static const int64_t kMinCp[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMinCp[n + 1] || cp > 0x10FFFF
        || (cp >= 0xD800 && cp <= 0xDFFF))
        return -1;
    len = n + 1;
    return cp;
}

bool VmExecutor::ExecuteIntrinsicString(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    //---- Phase 8e-1 protocol methods (migrated verbatim) ----
    case INTR_String_Equals:
    {
        std::string a = ReadStrArg(*this, locals, callParamBase, 0);
        std::string b = ReadStrArg(*this, locals, callParamBase, 1);
        int32_t result = (a == b) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_GetHashCode:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        int32_t hash = static_cast<int32_t>(
            std::hash<std::string>{}(s));
        std::memcpy(pResult, &hash, sizeof(hash));
        return true;
    }
    //---- Phase 11 Step 3 methods ----
    case INTR_String_Substring:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        int32_t start, end;
        std::memcpy(&start, locals + callParamBase + kFrameSlotBytes,
            sizeof(start));
        std::memcpy(&end, locals + callParamBase + 2 * kFrameSlotBytes,
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
        int32_t handle = MintNewString(std::move(out));
        std::memcpy(pResult, &handle, sizeof(handle));
        return true;
    }
    case INTR_String_IndexOf:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string needle = ReadStrArg(*this, locals,
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
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string prefix = ReadStrArg(*this, locals,
            callParamBase, 1);
        int32_t result = (s.size() >= prefix.size()
            && 0 == s.compare(0, prefix.size(), prefix)) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_EndsWith:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string suffix = ReadStrArg(*this, locals,
            callParamBase, 1);
        int32_t result = (s.size() >= suffix.size()
            && 0 == s.compare(s.size() - suffix.size(), suffix.size(),
                suffix)) ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_Contains:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string needle = ReadStrArg(*this, locals,
            callParamBase, 1);
        int32_t result = s.find(needle) != std::string::npos ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_ToUpper:
    case INTR_String_ToLower:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
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
        int32_t handle = MintNewString(std::move(s));   //s dead after the mint
        std::memcpy(pResult, &handle, sizeof(handle));
        return true;
    }
    case INTR_String_Trim:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        auto isWs = [](char ch) {
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'
                || ch == '\f' || ch == '\v';
        };
        size_t b = 0, e = s.size();
        while (b < e && isWs(s[b])) ++b;
        while (e > b && isWs(s[e - 1])) --e;
        std::string out = s.substr(b, e - b);
        int32_t handle = MintNewString(std::move(out));
        std::memcpy(pResult, &handle, sizeof(handle));
        return true;
    }
    case INTR_String_Split:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string sep = ReadStrArg(*this, locals,
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
            dst.push_back(AllocBoxedValue(RTK_String,
                MintNewString(std::move(part))));
        }
        std::memcpy(pResult, &listHeapIdx, sizeof(listHeapIdx));
        return true;
    }
    case INTR_String_Replace:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        std::string oldStr = ReadStrArg(*this, locals,
            callParamBase, 1);
        std::string newStr = ReadStrArg(*this, locals,
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
        int32_t handle = MintNewString(std::move(out));
        std::memcpy(pResult, &handle, sizeof(handle));
        return true;
    }
    case INTR_String_ToInt:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
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
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
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
    //---- 0.7.5 char bridge: code-point surfaces + strict parses ----
    case INTR_String_CharAt:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        int32_t idx;
        std::memcpy(&idx, locals + callParamBase + kFrameSlotBytes,
            sizeof(idx));
        if (idx < 0 || static_cast<size_t>(idx) >= s.size())
            RaiseNlangException(m_oobExcClassIdx,
                "string.charAt: byte index " + std::to_string(idx)
                + " outside string of " + std::to_string(s.size())
                + " bytes.");
        int len = 0;
        int64_t cp = DecodeCodePointAt(s, static_cast<size_t>(idx), len);
        if (cp < 0)
            RaiseNlangExceptionBase(
                "string.charAt: byte index " + std::to_string(idx)
                + " does not start a valid UTF-8 sequence.");
        uint32_t result = static_cast<uint32_t>(cp);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_CharCount:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        int32_t count = 0;
        size_t i = 0;
        while (i < s.size())
        {
            int len = 0;
            if (DecodeCodePointAt(s, i, len) < 0)
                RaiseNlangExceptionBase(
                    "string.charCount: invalid UTF-8 sequence at byte "
                    + std::to_string(i) + ".");
            ++count;
            i += static_cast<size_t>(len);
        }
        std::memcpy(pResult, &count, sizeof(count));
        return true;
    }
    case INTR_String_ToChar:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        //Strict parse family: the decimal CODE POINT, then the same
        //scalar validation as `n as char` (surrogate / > 0x10FFFF
        //reject) so both spellings agree.
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            RaiseNlangExceptionBase(
                "string.toChar: \"" + s + "\" is not a valid char.");
        errno = 0;
        char* end = nullptr;
        long long parsed = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE
            || parsed < 0 || parsed > 0x10FFFF
            || (parsed >= 0xD800 && parsed <= 0xDFFF))
            RaiseNlangExceptionBase(
                "string.toChar: \"" + s + "\" is not a valid char.");
        uint32_t result = static_cast<uint32_t>(parsed);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_String_ToLong:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            RaiseNlangExceptionBase(
                "string.toLong: \"" + s + "\" is not a valid long.");
        errno = 0;
        char* end = nullptr;
        long long parsed = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0' || errno == ERANGE)
            RaiseNlangExceptionBase(
                "string.toLong: \"" + s + "\" is not a valid long.");
        std::memcpy(pResult, &parsed, sizeof(parsed));
        return true;
    }
    case INTR_String_ToDouble:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        if (s.empty() || std::isspace(static_cast<unsigned char>(s[0])))
            RaiseNlangExceptionBase(
                "string.toDouble: \"" + s + "\" is not a valid double.");
        errno = 0;
        char* end = nullptr;
        double parsed = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0' || !std::isfinite(parsed))
            RaiseNlangExceptionBase(
                "string.toDouble: \"" + s + "\" is not a valid double.");
        std::memcpy(pResult, &parsed, sizeof(parsed));
        return true;
    }
    case INTR_String_ToBool:
    {
        std::string s = ReadStrArg(*this, locals, callParamBase, 0);
        //Strict: exactly "true"/"false". No case folding and no
        //garbage-as-false (Java's parseBoolean answers false for any
        //other input — a silent false hides typos).
        if (s != "true" && s != "false")
            RaiseNlangExceptionBase(
                "string.toBool: \"" + s + "\" is not a valid bool "
                "(expected \"true\" or \"false\").");
        int32_t result = (s == "true") ? 1 : 0;
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    default:
        return false;
    }
}

} //namespace nlang
