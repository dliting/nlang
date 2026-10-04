/*---
    VmExecutorOpsPrim.cpp - registry-instantiated dispatch tables and
    the executor handlers for the generalized numeric opcodes
    (OP_Add/Sub/Mul/Div/Mod/Neg/Cmp, OP_PrimCast, OP_Prim_to_str,
    OP_ConstInt64/OP_ConstDouble). Tables are pure arithmetic: every
    run-time check (integer zero divisor, invalid code point,
    unsupported kind) lives in the handlers below, which are VmExecutor
    members and can raise named errors.
---*/
#include "VmExecutor.h"
#include "VmPrimOps.h"
#include <nlang/runtime/RnTypes.h>
#include <cmath>
#include <cstring>

namespace nlang {

//Arithmetic per row. Bool/char rows compile their entries to nullptr —
//the resolver never emits arithmetic on them; a nullptr hit is a named
//executor error (handler-side).
template <ScalarPrimId ID>
struct ArithEntry
{
    using T = typename PrimCarrier<ID>::T;
    static constexpr ScalarPrimCategory C = kPrimCategory[ID];
    static constexpr bool kNumeric =
        (C == PC_SInt || C == PC_UInt || C == PC_Float);
    static constexpr bool kFloat = (C == PC_Float);

    static void AddImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) += *reinterpret_cast<const T*>(s); }
    static void SubImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) -= *reinterpret_cast<const T*>(s); }
    static void MulImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) *= *reinterpret_cast<const T*>(s); }
    //Integer and float rows share plain /= — the integer zero-divisor
    //check runs in the handler before dispatch; float follows IEEE.
    static void DivImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) /= *reinterpret_cast<const T*>(s); }
    static void ModInt(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) %= *reinterpret_cast<const T*>(s); }
    static void ModFloat(PrimSlot d, PrimConst s)
    {
        //static_cast back: fmod promotes float args to double; the
        //implicit narrowing would warn C4244 on T=float
        //(zero-new-warning gate).
        *reinterpret_cast<T*>(d) = static_cast<T>(
            std::fmod(*reinterpret_cast<T*>(d),
                      *reinterpret_cast<const T*>(s)));
    }
    static void NegImpl(PrimSlot d)
    { *reinterpret_cast<T*>(d) = -*reinterpret_cast<T*>(d); }

    static constexpr PrimBinFn Add = kNumeric ? &AddImpl : nullptr;
    static constexpr PrimBinFn Sub = kNumeric ? &SubImpl : nullptr;
    static constexpr PrimBinFn Mul = kNumeric ? &MulImpl : nullptr;
    static constexpr PrimBinFn Div = kNumeric ? &DivImpl : nullptr;
    static constexpr PrimBinFn Mod =
        kNumeric ? (kFloat ? &ModFloat : &ModInt) : nullptr;
    static constexpr PrimNegFn  Neg = kNumeric ? &NegImpl : nullptr;
};

#define ADD_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Add,
#define SUB_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Sub,
#define MUL_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Mul,
#define DIV_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Div,
#define MOD_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Mod,
#define NEG_ROW(Name, Kw, Width, Carrier, Cat, Rank) ArithEntry<SPR_##Name>::Neg,

const PrimBinFn kAddTable[] = { SCALAR_PRIMITIVE_DECL(ADD_ROW) };
const PrimBinFn kSubTable[] = { SCALAR_PRIMITIVE_DECL(SUB_ROW) };
const PrimBinFn kMulTable[] = { SCALAR_PRIMITIVE_DECL(MUL_ROW) };
const PrimBinFn kDivTable[] = { SCALAR_PRIMITIVE_DECL(DIV_ROW) };
const PrimBinFn kModTable[] = { SCALAR_PRIMITIVE_DECL(MOD_ROW) };
const PrimNegFn  kNegTable[] = { SCALAR_PRIMITIVE_DECL(NEG_ROW) };

//Comparison per row/op. Ordered rows (integers/floats/char — the
//uint32 code-point order gives char×char all six relational ops) carry
//every op; bool rows carry Eq/Ne only — relational order on a truth
//value is never emitted (resolver gate rejects it).
template <ScalarPrimId ID, int OP>
struct CmpEntry
{
    using T = typename PrimCarrier<ID>::T;
    static constexpr ScalarPrimCategory C = kPrimCategory[ID];
    static constexpr bool kOrdered =
        (C == PC_SInt || C == PC_UInt || C == PC_Float || C == PC_Char);
    static constexpr bool kEqNeOnly = (C == PC_Bool);

    static int Run(PrimConst l, PrimConst r)
    {
        T a = *reinterpret_cast<const T*>(l);
        T b = *reinterpret_cast<const T*>(r);
        switch (OP)  // compile-time constant — folds to one comparison
        {
        case 0: return a <  b;
        case 1: return a <= b;
        case 2: return a >  b;
        case 3: return a >= b;
        case 4: return a == b;
        default: return a != b;
        }
    }

    static constexpr PrimCmpFn Fn =
        (kOrdered || (kEqNeOnly && (OP == 4 || OP == 5))) ? &Run : nullptr;
};

#define CMP0(Name, ...) CmpEntry<SPR_##Name, 0>::Fn,
#define CMP1(Name, ...) CmpEntry<SPR_##Name, 1>::Fn,
#define CMP2(Name, ...) CmpEntry<SPR_##Name, 2>::Fn,
#define CMP3(Name, ...) CmpEntry<SPR_##Name, 3>::Fn,
#define CMP4(Name, ...) CmpEntry<SPR_##Name, 4>::Fn,
#define CMP5(Name, ...) CmpEntry<SPR_##Name, 5>::Fn,
const PrimCmpFn kCmpTable[6][SPR_Count] =
{
    { SCALAR_PRIMITIVE_DECL(CMP0) },
    { SCALAR_PRIMITIVE_DECL(CMP1) },
    { SCALAR_PRIMITIVE_DECL(CMP2) },
    { SCALAR_PRIMITIVE_DECL(CMP3) },
    { SCALAR_PRIMITIVE_DECL(CMP4) },
    { SCALAR_PRIMITIVE_DECL(CMP5) },
};

//Cast per src/dst pair. bool rows/cols are nullptr (never emitted);
//char <-> numeric cells are plain static_cast — the numeric->char code
//point validity is pre-checked by the handler (registry-driven slot
//read), so invalid values raise a named error before this table runs.
//Numeric pairs: static_cast = C# unchecked semantics (truncation,
//round-to-nearest on int->float), matching the legacy cast behavior.
template <ScalarPrimId S, ScalarPrimId D>
struct CastCell
{
    using ST = typename PrimCarrier<S>::T;
    using DT = typename PrimCarrier<D>::T;
    static constexpr ScalarPrimCategory SC = kPrimCategory[S];
    static constexpr ScalarPrimCategory DC = kPrimCategory[D];

    static void NumImpl(PrimSlot p)
    {
        *reinterpret_cast<DT*>(p) =
            static_cast<DT>(*reinterpret_cast<const ST*>(p));
    }

    static constexpr PrimCastFn Fn =
        (SC == PC_Bool || DC == PC_Bool) ? nullptr : &NumImpl;
};

//File-static mutable storage filled once at static init (a const
//array could not be dynamically filled); the header exposes the
//read-only accessor PrimCastCell(srcRow, dstRow).
static PrimCastFn s_PrimCastTable[SPR_Count][SPR_Count];

template <ScalarPrimId S>
static void FillCastRow(PrimCastFn row[])
{
#define CAST_CELL(Name, Kw, Width, Carrier, Cat, Rank) \
    row[SPR_##Name] = CastCell<S, SPR_##Name>::Fn;
    SCALAR_PRIMITIVE_DECL(CAST_CELL)
#undef CAST_CELL
}

template <ScalarPrimId S>
static void InitCastRows()
{
    FillCastRow<S>(s_PrimCastTable[S]);
    if constexpr (static_cast<int>(S) + 1 < SPR_Count)
        InitCastRows<static_cast<ScalarPrimId>(S + 1)>();
}

static const bool s_PrimCastTableInit = [] {
    //MUST start at the first registry row (SPR_Int32 = 0), not the
    //first NEW row — starting at SPR_Byte left the legacy Int32/Float
    //cells null and every int<->float conversion raised at run time.
    InitCastRows<SPR_Int32>();
    return true;
}();

PrimCastFn PrimCastCell(int srcRow, int dstRow)
{
    if (srcRow < 0 || dstRow < 0 || srcRow >= SPR_Count || dstRow >= SPR_Count)
        return nullptr;
    return s_PrimCastTable[srcRow][dstRow];
}

static_assert(sizeof(kAddTable) / sizeof(kAddTable[0]) == SPR_Count);
static_assert(sizeof(kNegTable) / sizeof(kNegTable[0]) == SPR_Count);

//--- Executor handlers -------------------------------------------------

//Registry-driven slot read for the numeric->char code-point check:
//signed rows sign-extend by width, unsigned rows zero-extend, float
//rows truncate toward zero first.
int64_t VmExecutor::ReadScalarAsInt64(int row, const uint8_t* p) const
{
    const auto& prim = kScalarPrims[row];
    switch (prim.category)
    {
    case PC_UInt:
        switch (prim.slotWidth)
        {
        case 1: return *reinterpret_cast<const uint8_t*>(p);
        case 2: return *reinterpret_cast<const uint16_t*>(p);
        case 4: return *reinterpret_cast<const uint32_t*>(p);
        default: return static_cast<int64_t>(
            *reinterpret_cast<const uint64_t*>(p));
        }
    case PC_Float:
        return prim.slotWidth == 8
            ? static_cast<int64_t>(*reinterpret_cast<const double*>(p))
            : static_cast<int64_t>(*reinterpret_cast<const float*>(p));
    default:  // PC_SInt (bool/char never reach here — cast cells are null)
        switch (prim.slotWidth)
        {
        case 1: return *reinterpret_cast<const int8_t*>(p);
        case 2: return *reinterpret_cast<const int16_t*>(p);
        case 4: return *reinterpret_cast<const int32_t*>(p);
        default: return *reinterpret_cast<const int64_t*>(p);
        }
    }
}

//True when all width bytes of the slot are zero — covers the integer
//zero divisor and the float +-0.0 divisor alike (the legacy OP_Div_f32
//raised on b == 0.0f, so bit-zero is the behavior-equivalent test).
static bool SlotIsZero(const uint8_t* p, uint8_t width)
{
    for (uint8_t i = 0; i < width; ++i)
        if (p[i]) return false;
    return true;
}

void VmExecutor::OpAdd(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16(), src = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kAddTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: add on unsupported kind");
    kAddTable[i](locals + dst, locals + src);
}

void VmExecutor::OpSub(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16(), src = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kSubTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: sub on unsupported kind");
    kSubTable[i](locals + dst, locals + src);
}

void VmExecutor::OpMul(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16(), src = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kMulTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: mul on unsupported kind");
    kMulTable[i](locals + dst, locals + src);
}

void VmExecutor::OpDiv(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16(), src = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kDivTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: div on unsupported kind");
    if (SlotIsZero(locals + src, kScalarPrims[i].slotWidth))
        RaiseNlangException(m_divZeroExcClassIdx,
            "NLang VM: division by zero");
    kDivTable[i](locals + dst, locals + src);
}

void VmExecutor::OpMod(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16(), src = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kModTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: mod on unsupported kind");
    if (SlotIsZero(locals + src, kScalarPrims[i].slotWidth))
        RaiseNlangException(m_divZeroExcClassIdx,
            "NLang VM: modulo by zero");
    kModTable[i](locals + dst, locals + src);
}

void VmExecutor::OpNeg(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte();
    uint16_t dst = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || !kNegTable[i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: neg on unsupported kind");
    kNegTable[i](locals + dst);
}

void VmExecutor::OpCmp(BytecodeReader& r, uint8_t* locals)
{
    uint8_t kind = r.ReadByte(), cmpOp = r.ReadByte();
    uint16_t lhs = r.ReadUint16(), rhs = r.ReadUint16();
    int i = ScalarPrimIndexOfRtk(kind);
    if (i < 0 || cmpOp > 5 || !kCmpTable[cmpOp][i])
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: compare on unsupported kind");
    int v = kCmpTable[cmpOp][i](locals + lhs, locals + rhs);
    //bool result: 4-byte 0/1 written back into the lhs slot (same
    //convention as the legacy OP_Less_i32 family).
    uint32_t b = v ? 1u : 0u;
    std::memcpy(locals + lhs, &b, sizeof(b));
}

void VmExecutor::OpPrimCast(BytecodeReader& r, uint8_t* pResult)
{
    uint8_t sk = r.ReadByte(), dk = r.ReadByte();
    int si = ScalarPrimIndexOfRtk(sk), di = ScalarPrimIndexOfRtk(dk);
    PrimCastFn fn = PrimCastCell(si, di);   // accessor guards row bounds
    if (!fn)
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: invalid primitive cast");
    //numeric -> char: validate the Unicode scalar value BEFORE
    //converting (fn non-null implies both row indices are in bounds).
    if (kScalarPrims[di].category == PC_Char)
    {
        int64_t v = ReadScalarAsInt64(si, pResult);
        if (v < 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
            RaiseNlangException(m_exceptionClassIdx,
                "NLang VM: value is not a valid Unicode scalar value");
    }
    fn(pResult);
}

//Scalar -> string, registry-driven: format via the kind's
//ValueToString (bool "true"/"false", char one UTF-8 code point,
//integers decimal, floats %g/%.17g) and mint a string object the same
//way the retired OP_Int32_to_str/OP_Float_to_str did.
void VmExecutor::OpPrimToStr(BytecodeReader& r, uint8_t* pResult)
{
    int i = ScalarPrimIndexOfRtk(r.ReadByte());
    if (i < 0)
        RaiseNlangException(m_exceptionClassIdx,
            "NLang VM: to-string on unsupported kind");
    //Formatting rides the registry-generated Rn singletons (per-category
    //ValueToString). Their construction interns names through IdString —
    //the host must have run Runtime::StaticInit() first (nvm/ndb do;
    //in-process test hosts already did).
    std::string s = RnBuiltinDataType::InstanceOf(kScalarPrims[i].kind)
                        ->ValueToString(pResult);
    int32_t handle = MintNewString(std::move(s));
    std::memcpy(pResult, &handle, sizeof(handle));
}

void VmExecutor::OpConstInt64(BytecodeReader& r, uint8_t* pResult)
{
    int64_t v;
    std::memcpy(&v, r.ReadBytes(sizeof(v)), sizeof(v));
    std::memcpy(pResult, &v, sizeof(v));
}

void VmExecutor::OpConstDouble(BytecodeReader& r, uint8_t* pResult)
{
    double v;
    std::memcpy(&v, r.ReadBytes(sizeof(v)), sizeof(v));
    std::memcpy(pResult, &v, sizeof(v));
}

} // namespace nlang
