/*---
    VmExecutorOpsStrings.cpp — 字符串操作码（拼接/比较/长度/枚举与数组渲染）
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpEnum_to_str(BytecodeReader& reader, uint8_t* pResult) {
    //uint16 enumDefIdx immediate; read int32 enum value from pResult,
    //lookup m_currModule->enumNames[enumDefIdx][value], push name.
    uint16_t enumDefIdx = reader.ReadUint16();
    int32_t enumValue;
    std::memcpy(&enumValue, pResult, sizeof(enumValue));
    if (enumDefIdx >= m_currModule->enumNames.size())
        throw std::runtime_error(
            "NLang VM: enum def idx out of range");
    const auto& names = m_currModule->enumNames[enumDefIdx];
    if (enumValue < 0
        || static_cast<size_t>(enumValue) >= names.size())
        throw std::runtime_error(
            "NLang VM: enum value out of range");
    int32_t handle = MintNewString(names[static_cast<size_t>(enumValue)]);
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpArray_to_str(uint8_t* pResult) {
    //Phase 9b-pre: format array at pResult as "[e1, e2, ...]",
    //push result string to pool, write idx to pResult.
    //Reads heap layout: slot[0]=RTK_Array, slot[1]=elemKind,
    //slot[2]=length, slot[3+i]=elements.
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    std::string s;
    if (heapIdx <= 0) {
        s = "<null>";
    } else if (static_cast<size_t>(heapIdx) >= m_structHeap.size()) {
        throw std::runtime_error(
            "NLang VM: array_to_str on stale reference");
    } else if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array) {
        throw std::runtime_error(
            "NLang VM: array_to_str on non-array");
    } else {
        s = FormatArray(heapIdx, 0);
    }
    int32_t handle = MintNewString(std::move(s));
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpConcat_str(BytecodeReader& reader, uint8_t* locals) {
    //Transparent cons (Task 3): O(1) zero-copy node, flattened
    //lazily on first read (iteratively, in place — see StrVal).
    //n appends cost n nodes, all collectable, instead of n O(n)
    //copies. Null/dead operand handles stay 0 and contribute
    //nothing at flatten time — same "" fallback as before.
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t hA, hB;
    std::memcpy(&hA, locals + dst, sizeof(hA));
    std::memcpy(&hB, locals + src, sizeof(hB));
    if (hA == hB) {
        //Self-concat (s = s + s) would link the same handle on
        //both sides. The walk still terminates (cons edges only
        //ever point at older nodes, so no true cycle can form),
        //but the shared subtree is revisited once per occurrence
        //— and repeated self-doubling compounds that into 2^n
        //visits. Materialize the operand instead: rare shape,
        //correctness first.
        std::string doubled = StrVal(hA);   //no mint between reads
        doubled += StrVal(hA);
        int32_t handle = MintNewString(std::move(doubled));
        std::memcpy(locals + dst, &handle, sizeof(handle));
        return;
    }
    int32_t handle = AllocConsString(hA, hB);
    std::memcpy(locals + dst, &handle, sizeof(handle));
}

void VmExecutor::OpEq_str(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t hA, hB;
    std::memcpy(&hA, locals + lhs, sizeof(hA));
    std::memcpy(&hB, locals + rhs, sizeof(hB));
    int32_t r = StringsEqual(hA, hB) ? 1 : 0;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpNe_str(BytecodeReader& reader, uint8_t* locals) {
    uint16_t lhs = reader.ReadUint16();
    uint16_t rhs = reader.ReadUint16();
    int32_t hA, hB;
    std::memcpy(&hA, locals + lhs, sizeof(hA));
    std::memcpy(&hB, locals + rhs, sizeof(hB));
    int32_t r = StringsEqual(hA, hB) ? 0 : 1;
    std::memcpy(locals + lhs, &r, sizeof(r));
}

void VmExecutor::OpStrLen(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t handle;
    std::memcpy(&handle, locals + src, sizeof(handle));
    //Byte length by design (Phase 11 decision #7, Go/Lua
    //model): substring/indexOf use byte offsets too, so
    //length stays consistent with them. "héllo".length()==6.
    int32_t len = static_cast<int32_t>(StrVal(handle).size());
    std::memcpy(locals + dst, &len, sizeof(len));
}

//0.7.5 char bridge: s[i] — read the idx-th BYTE as ubyte. Byte semantics
//match length/substring/indexOf (the code-point surfaces are foreach and
//charAt); a null string handle reads "" and fails the range check with
//the same catchable OOB exception as arrays.
void VmExecutor::OpStrByteAt(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t str = reader.ReadUint16();
    uint16_t idx = reader.ReadUint16();
    int32_t handle, index;
    std::memcpy(&handle, locals + str, sizeof(handle));
    std::memcpy(&index, locals + idx, sizeof(index));
    const std::string& s = StrVal(handle);
    if (index < 0 || static_cast<size_t>(index) >= s.size())
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: string index out of range");
    int32_t byte = static_cast<uint8_t>(
        s[static_cast<size_t>(index)]);
    std::memcpy(locals + dst, &byte, sizeof(byte));
}

//0.7.5 char bridge: one code-point iteration step (the whole foreach
//lowering is this op at the loop head). Decode shape mirrors the lexer's
//DecodeCharLiteral: minimal length, continuation bytes, overlong /
//surrogate / >0x10FFFF rejections. A malformed sequence raises the
//catchable base Exception like the other string methods (spec §4.4):
//io.readFile can load arbitrary bytes, so invalid UTF-8 is reachable
//from pure NLang code and must not be an uncatchable internal error.
void VmExecutor::OpStrForeachStep(BytecodeReader& reader, uint8_t* locals) {
    uint16_t str = reader.ReadUint16();
    uint16_t off = reader.ReadUint16();
    uint16_t cond = reader.ReadUint16();
    uint16_t ch = reader.ReadUint16();
    int32_t handle, offset;
    std::memcpy(&handle, locals + str, sizeof(handle));
    std::memcpy(&offset, locals + off, sizeof(offset));
    int32_t more;
    const std::string& s = StrVal(handle);
    if (offset < 0 || static_cast<size_t>(offset) >= s.size()) {
        more = 0;  // past the byte length — loop exit flag
    } else {
        const size_t pos = static_cast<size_t>(offset);
        const unsigned char b0 = static_cast<unsigned char>(s[pos]);
        unsigned cp;
        int len;
        if (b0 < 0x80)                { cp = b0;          len = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F;   len = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F;   len = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07;   len = 4; }
        else RaiseNlangExceptionBase(
                 "string iteration: invalid UTF-8 sequence.");
        if (pos + static_cast<size_t>(len) > s.size())
            RaiseNlangExceptionBase(
                "string iteration: invalid UTF-8 sequence.");
        for (int i = 1; i < len; ++i) {
            const unsigned char bi =
                static_cast<unsigned char>(s[pos + static_cast<size_t>(i)]);
            if ((bi & 0xC0) != 0x80)
                RaiseNlangExceptionBase(
                    "string iteration: invalid UTF-8 sequence.");
            cp = (cp << 6) | (bi & 0x3F);
        }
        static const unsigned kMinCp[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (cp < kMinCp[len] || cp > 0x10FFFF
            || (cp >= 0xD800 && cp <= 0xDFFF))
            RaiseNlangExceptionBase(
                "string iteration: invalid UTF-8 sequence.");
        int32_t codePoint = static_cast<int32_t>(cp);
        std::memcpy(locals + ch, &codePoint, sizeof(codePoint));
        offset += len;
        std::memcpy(locals + off, &offset, sizeof(offset));
        more = 1;
    }
    std::memcpy(locals + cond, &more, sizeof(more));
}

} // namespace nlang
