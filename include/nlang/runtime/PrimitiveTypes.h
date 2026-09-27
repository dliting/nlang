/*--- PrimitiveTypes.h - the single source of truth for the 12 scalar
    primitives. One row per type; every consumer (frame slot widths,
    instruction kind immediates, serialization kinds, cast derivation,
    numeric promotion, debug formatting) derives from this table.
    Adding a future type (fp16/bf16) is one row plus a carrier typedef. ---*/
#ifndef NLANG_RUNTIME_PRIMITIVE_TYPES_H
#define NLANG_RUNTIME_PRIMITIVE_TYPES_H

#include "NodeConsts.h"
#include "TypeDef.h"   // repo int8/uint8/int32/... typedefs (no _t suffix, same idiom as RnTypes)

#include <cstdint>
#include <cstddef>
#include <string>

namespace nlang
{

//Scalar primitive categories. Integers split by signedness so the
//implicit-conversion derivation can reason about value domains.
enum ScalarPrimCategory : uint8_t
{
    PC_SInt,    // byte short int long
    PC_UInt,    // ubyte ushort uint ulong
    PC_Float,   // float double
    PC_Bool,    // bool
    PC_Char     // char (Unicode scalar value)
};

//Registry row. rank = promotion order within the category
//(SI/UI: 1..4 by width; F: 1..2 by width; B/C: 1).
//Carrier typedefs use the repo TypeDef.h forms (int8, not int8_t) so
//RnBuiltinDataTypeT<NK_X, carrier> instantiations match existing idiom.
//The full 12-row table below is the single source; macro consumers that
//must skip the two pre-existing hand-written Rn/Sn classes (Int32,
//Float) use SCALAR_PRIMITIVE_NEW_DECL instead.
#define SCALAR_PRIMITIVE_DECL(M)                                        \
    M(Int32,  "int",    4, int32,   PC_SInt, 3)                         \
    M(Float,  "float",  4, float,   PC_Float, 1)                        \
    M(Byte,   "byte",   1, int8,    PC_SInt, 1)                         \
    M(UByte,  "ubyte",  1, uint8,   PC_UInt, 1)                         \
    M(Short,  "short",  2, int16,   PC_SInt, 2)                         \
    M(UShort, "ushort", 2, uint16,  PC_UInt, 2)                         \
    M(UInt32, "uint",   4, uint32,  PC_UInt, 3)                         \
    M(Long,   "long",   8, int64,   PC_SInt, 4)                         \
    M(ULong,  "ulong",  8, uint64,  PC_UInt, 4)                         \
    M(Double, "double", 8, double,  PC_Float, 2)                        \
    M(Bool,   "bool",   4, int32,   PC_Bool, 1)                         \
    M(Char,   "char",   4, uint32,  PC_Char, 1)

#define SCALAR_PRIMITIVE_NEW_DECL(M)   /* all but Int32/Float */        \
    M(Byte,   "byte",   1, int8,    PC_SInt, 1)                         \
    M(UByte,  "ubyte",  1, uint8,   PC_UInt, 1)                         \
    M(Short,  "short",  2, int16,   PC_SInt, 2)                         \
    M(UShort, "ushort", 2, uint16,  PC_UInt, 2)                         \
    M(UInt32, "uint",   4, uint32,  PC_UInt, 3)                         \
    M(Long,   "long",   8, int64,   PC_SInt, 4)                         \
    M(ULong,  "ulong",  8, uint64,  PC_UInt, 4)                         \
    M(Double, "double", 8, double,  PC_Float, 2)                        \
    M(Bool,   "bool",   4, int32,   PC_Bool, 1)                         \
    M(Char,   "char",   4, uint32,  PC_Char, 1)

struct ScalarPrimInfo
{
    NodeKind            kind;       // NK_Byte .. NK_Char
    uint8_t             rtk;        // RTK_Byte .. RTK_Char (see below)
    uint8_t             slotWidth;  // frame slot size in bytes
    ScalarPrimCategory  category;
    uint8_t             rank;       // within category
    const char*         name;       // language keyword
};

//RTK codes for scalar primitives — the serialization kind AND the
//instruction kind immediate. Defined here (runtime layer) so compiler
//and VM share one numbering; CompiledModule.h keeps the non-scalar
//RTK_* constants (String/Struct/Class/Array/Boxed/Func), and
//TypeDesc.h's descriptor-only RTK_List/RTK_Dict keep 8/9 — new scalars
//start at 10 to stay clear of both (2026-09-27 review finding).
static constexpr uint8_t RTK_Int32  = 0;   // legacy wire values, do not renumber
static constexpr uint8_t RTK_Float  = 1;   // legacy wire values, do not renumber
static constexpr uint8_t RTK_Byte   = 10;
static constexpr uint8_t RTK_UByte  = 11;
static constexpr uint8_t RTK_Short  = 12;
static constexpr uint8_t RTK_UShort = 13;
static constexpr uint8_t RTK_UInt32 = 14;
static constexpr uint8_t RTK_Long   = 15;
static constexpr uint8_t RTK_ULong  = 16;
static constexpr uint8_t RTK_Double = 17;
static constexpr uint8_t RTK_Bool   = 18;
static constexpr uint8_t RTK_Char   = 19;
//Range is non-dense (0,1 scalars; 2..7 legacy non-scalars; 8,9
//descriptor-only; 10..19 scalars) — membership tests go through
//ScalarPrimIndexOfRtk, never a range compare.

//Registry access. Index order == SCALAR_PRIMITIVE_DECL order.
extern const ScalarPrimInfo kScalarPrims[];
extern const size_t         kScalarPrimCount;

//-1 when the kind is not one of the 12 scalars (string/struct/...).
int ScalarPrimIndexOf(NodeKind kind);
//-1 when the rtk is not a scalar kind (0=Int32,1=Float are scalars;
//2..7 are string/struct/... and return -1).
int ScalarPrimIndexOfRtk(uint8_t rtk);

inline uint8_t RtkOfKind(NodeKind kind)
{
    int i = ScalarPrimIndexOf(kind);
    return i < 0 ? 0xFF : kScalarPrims[i].rtk;
}

//Scalar kinds never hold heap references — derived from category.
bool PrimKindIsTraceable(NodeKind kind);

//Numeric = integer or float category (bool/char are not numeric).
inline bool PrimCategoryIsNumeric(ScalarPrimCategory c)
{
    return c == PC_SInt || c == PC_UInt || c == PC_Float;
}
bool PrimKindIsNumeric(NodeKind kind);

//Encode one Unicode scalar value as its 1-4 byte UTF-8 sequence.
//Shared by RnChar::ValueToString here and the char->string bridge in
//the VM (0.7.5 Task 6); surrogate-pair validity is the caller's job.
inline std::string Utf8EncodeCodePoint(uint32 cp)
{
    std::string out;
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

} // namespace nlang

#endif // NLANG_RUNTIME_PRIMITIVE_TYPES_H
