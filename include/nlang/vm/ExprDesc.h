#pragma once
#include "CompiledModule.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace nlang {

//--- ExprDesc mini expression grammar (v2.3) ------------------------------
//Serialized default-value expressions. One subtree per byte stream:
//
//  ExprDesc := u8 tag
//    RTK_Int32/RTK_Float/RTK_Long/RTK_ULong/RTK_Double  scalar payload
//                (same encoding as the DefaultValueDesc channels above)
//    RTK_String   + u32 stringConstants index (producer pool)
//    RTK_Null     (no payload)
//    ED_FormalRef + u8 formal index (an earlier formal of the same
//                 function; the consumer's forward-ref check enforces it)
//    ED_Neg       + 1 child ExprDesc (unary negation)
//    ED_Binary    + u8 op code (EDB_*) + 2 child ExprDescs
//    ED_Call      + u16 keyLen + qualified key bytes ("<module>.<func>")
//                 + u32 paramCount + u8 argCount + argCount child ExprDescs
//
//Anything the source module writes outside this grammar stays
//RTK_Unfoldable (unchanged consumer-side reject).
inline constexpr uint8_t ED_FormalRef = 0x20;
inline constexpr uint8_t ED_Neg       = 0x21;
inline constexpr uint8_t ED_Binary    = 0x22;
inline constexpr uint8_t ED_Call      = 0x23;
//Binary op codes, in SnBinaryExpr::Operator order (+ - * / % << >>).
inline constexpr uint8_t EDB_Add = 0;
inline constexpr uint8_t EDB_Sub = 1;
inline constexpr uint8_t EDB_Mul = 2;
inline constexpr uint8_t EDB_Div = 3;
inline constexpr uint8_t EDB_Mod = 4;
inline constexpr uint8_t EDB_Shl = 5;
inline constexpr uint8_t EDB_Shr = 6;
//Depth/size ceilings shared by the writer (out-of-subset fallback), the
//link-time remap and the consumer rebuild (malformed-module reject).
inline constexpr int kMaxExprDescDepth = 16;
inline constexpr size_t kMaxExprDescBytes = 4096;

//Bounds-checked little-endian reader over an ExprDesc byte stream. Every
//read either lands fully inside the stream or fails: a truncated or
//garbage stream surfaces as false, never as an out-of-bounds read.
struct ExprDescCursor {
    const uint8_t *pData = nullptr;
    size_t size = 0;
    size_t offset = 0;

    explicit ExprDescCursor(const std::vector<uint8_t> &bytes)
        : pData(bytes.data()), size(bytes.size()) {}

    bool AtEnd() const { return offset == size; }

    bool U8(uint8_t &v)   { return Bytes(&v, sizeof(v)); }
    bool U16(uint16_t &v) { return Bytes(&v, sizeof(v)); }
    bool U32(uint32_t &v) { return Bytes(&v, sizeof(v)); }
    bool I64(int64_t &v)  { return Bytes(&v, sizeof(v)); }
    bool F32(float &v)    { return Bytes(&v, sizeof(v)); }
    bool F64(double &v)   { return Bytes(&v, sizeof(v)); }
    bool Bytes(void *dst, size_t n)
    {
        if (n > size - offset) return false;
        std::memcpy(dst, pData + offset, n);
        offset += n;
        return true;
    }
    bool Skip(size_t n)
    {
        if (n > size - offset) return false;
        offset += n;
        return true;
    }
};

//ED_Call arm of the remap: validate the call header, then remap the
//children. Split out to keep the tag switch a single screen. (Mutually
//recursive with the tag switch below — forward-declared.)
inline bool ExprDescRemapStrings(std::vector<uint8_t> &bytes,
    const std::function<uint32_t(uint32_t)> &lookup, int depth,
    ExprDescCursor &cur);
inline bool ExprDescRemapCall(std::vector<uint8_t> &bytes,
    const std::function<uint32_t(uint32_t)> &lookup, int depth,
    ExprDescCursor &cur)
{
    uint16_t keyLen = 0;
    uint32_t paramCount = 0;
    uint8_t argCount = 0;
    if (!cur.U16(keyLen) || !cur.Skip(keyLen)
        || !cur.U32(paramCount) || !cur.U8(argCount))
        return false;
    for (uint8_t i = 0; i < argCount; ++i)
        if (!ExprDescRemapStrings(bytes, lookup, depth + 1, cur))
            return false;
    return true;
}

//v2.3 link-time support: remap every ED_Literal_String producer-pool
//index in place through lookup (NcuLinker::FinalizeFunction, mirroring
//the RTK_String stringIdx remap). False = malformed stream — callers
//reject the image rather than link a corrupt default.
inline bool ExprDescRemapStrings(std::vector<uint8_t> &bytes,
    const std::function<uint32_t(uint32_t)> &lookup, int depth,
    ExprDescCursor &cur)
{
    if (depth > kMaxExprDescDepth) return false;
    uint8_t tag = 0;
    if (!cur.U8(tag)) return false;
    switch (tag)
    {
        case RTK_Int32:  { uint32_t v; return cur.U32(v); }
        case RTK_Float:  { float v;    return cur.F32(v); }
        case RTK_Long:
        case RTK_ULong:  { int64_t v;  return cur.I64(v); }
        case RTK_Double: { double v;   return cur.F64(v); }
        case RTK_String:
        {
            uint32_t idx = 0;
            if (!cur.U32(idx)) return false;
            const uint32_t mapped = lookup(idx);
            std::memcpy(bytes.data() + cur.offset - sizeof(mapped),
                        &mapped, sizeof(mapped));
            return true;
        }
        case RTK_Null:     return true;
        case ED_FormalRef: { uint8_t v; return cur.U8(v); }
        case ED_Neg:
            return ExprDescRemapStrings(bytes, lookup, depth + 1, cur);
        case ED_Binary:
        {
            uint8_t op = 0;
            if (!cur.U8(op) || op > EDB_Shr) return false;
            return ExprDescRemapStrings(bytes, lookup, depth + 1, cur)
                && ExprDescRemapStrings(bytes, lookup, depth + 1, cur);
        }
        case ED_Call:
            return ExprDescRemapCall(bytes, lookup, depth, cur);
        default:
            return false;
    }
}

inline bool ExprDescRemapStrings(std::vector<uint8_t> &bytes,
    const std::function<uint32_t(uint32_t)> &lookup)
{
    if (bytes.empty()) return true;
    ExprDescCursor cur(bytes);
    if (!ExprDescRemapStrings(bytes, lookup, 0, cur)) return false;
    return cur.AtEnd();
}

}  // namespace nlang
