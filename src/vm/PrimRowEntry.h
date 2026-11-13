/*---
    PrimRowEntry.h - the per-row numeric entry template shared by the
    opcode-family table TUs (VmExecutorOpsPrim.cpp: arithmetic,
    shifts, comparison, cast; VmExecutorOpsBit.cpp: the 0.13.7
    bitwise family). ArithEntry<Row> exposes the row’s member
    implementations plus the constexpr function-pointer constants
    the dispatch tables are instantiated from.
---*/
#pragma once
#include "VmPrimOps.h"
#include <nlang/runtime/RnTypes.h>
#include <cmath>
#include <type_traits>

namespace nlang {

//One entry per numeric registry row, instantiated per opcode-family table
//TU. Bool/char rows compile the non-applicable entries to nullptr — the
//resolver never emits those kinds for the op; a nullptr hit is a named
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
    //Integer rows share plain /= — the integer zero-divisor check runs
    //in the handler before dispatch; float follows IEEE. x86 idiv also
    //traps on MIN/-1 (STATUS_INTEGER_OVERFLOW): route that one divisor
    //through the unsigned carrier so it wraps to MIN — the two's-
    //complement negation result, the shape Java's idiv returns.
    static void DivImpl(PrimSlot d, PrimConst s)
    {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            if (*reinterpret_cast<const T*>(s) == T(-1)) {
                using U = std::make_unsigned_t<T>;
                *reinterpret_cast<T*>(d) = static_cast<T>(
                    static_cast<U>(0)
                    - static_cast<U>(*reinterpret_cast<T*>(d)));
                return;
            }
        }
        *reinterpret_cast<T*>(d) /= *reinterpret_cast<const T*>(s);
    }
    //MIN % -1 mathematically yields remainder 0; the same idiv trap
    //applies, so answer it directly for signed rows.
    static void ModInt(PrimSlot d, PrimConst s)
    {
        if constexpr (std::is_signed_v<T>) {
            if (*reinterpret_cast<const T*>(s) == T(-1)) {
                *reinterpret_cast<T*>(d) = 0;
                return;
            }
        }
        *reinterpret_cast<T*>(d) %= *reinterpret_cast<const T*>(s);
    }
    static void ModFloat(PrimSlot d, PrimConst s)
    {
        //static_cast back: fmod promotes float args to double; the
        //implicit narrowing would warn C4244 on T=float
        //(zero-new-warning gate).
        *reinterpret_cast<T*>(d) = static_cast<T>(
            std::fmod(*reinterpret_cast<T*>(d),
                      *reinterpret_cast<const T*>(s)));
    }
    //Negation of INT_MIN is signed-overflow UB by the letter (x86 neg
    //never faults, but the shl row already set the unsigned-carrier
    //precedent): route signed integer rows through it — the two's-
    //complement result is exactly -(unsigned)value.
    static void NegImpl(PrimSlot d)
    {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            using U = std::make_unsigned_t<T>;
            *reinterpret_cast<T*>(d) = static_cast<T>(
                static_cast<U>(0)
                - static_cast<U>(*reinterpret_cast<T*>(d)));
        } else {
            *reinterpret_cast<T*>(d) = -*reinterpret_cast<T*>(d);
        }
    }

    //0.9.9 shifts, integer rows only (kInt gates; float/bool/char rows
    //compile to nullptr — the resolver rejects them compile-time).
    //`<<` always routes through the unsigned carrier: unsigned
    //wrap-around is exactly the two's-complement result the row must
    //store back (1 << 31 on int32 = INT32_MIN), with no reliance on
    //signed-shift corner behavior.
    //`>>` deliberately keeps the plain operator so the ROW'S OWN
    //signedness picks the flavor: arithmetic (sign-extending) on
    //signed rows — MSVC documents signed right shift as arithmetic —
    //and logical (zero-fill) on unsigned rows. Both flavors are pinned
    //by e2e (shift_types.n: -8 >> 1 == -4, 4294967288u >> 1 ==
    //2147483644), so a compiler flavor change cannot pass silently.
    static void ShlImpl(PrimSlot d, PrimConst s)
    {
        using U = std::make_unsigned_t<T>;
        T cnt = *reinterpret_cast<const T*>(s);
        *reinterpret_cast<T*>(d) = static_cast<T>(
            static_cast<U>(*reinterpret_cast<const T*>(d)) << cnt);
    }
    static void ShrImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) >>= *reinterpret_cast<const T*>(s); }

    //0.13.7 bitwise ops, integer rows only (same kInt gate as the
    //shifts; the resolver rejects every other kind at compile time).
    //Unlike shl, bit ops on the signed carrier have no overflow corner:
    //plain operators directly, no unsigned routing.
    static void BitAndImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) &= *reinterpret_cast<const T*>(s); }
    static void BitOrImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) |= *reinterpret_cast<const T*>(s); }
    static void BitXorImpl(PrimSlot d, PrimConst s)
    { *reinterpret_cast<T*>(d) ^= *reinterpret_cast<const T*>(s); }
    static void BitNotImpl(PrimSlot d)
    { *reinterpret_cast<T*>(d) =
        static_cast<T>(~*reinterpret_cast<const T*>(d)); }

    static constexpr PrimBinFn Add = kNumeric ? &AddImpl : nullptr;
    static constexpr PrimBinFn Sub = kNumeric ? &SubImpl : nullptr;
    static constexpr PrimBinFn Mul = kNumeric ? &MulImpl : nullptr;
    static constexpr PrimBinFn Div = kNumeric ? &DivImpl : nullptr;
    static constexpr PrimBinFn Mod =
        kNumeric ? (kFloat ? &ModFloat : &ModInt) : nullptr;
    static constexpr PrimNegFn  Neg = kNumeric ? &NegImpl : nullptr;
    static constexpr bool kInt = (C == PC_SInt || C == PC_UInt);
    static constexpr PrimBinFn Shl = kInt ? &ShlImpl : nullptr;
    static constexpr PrimBinFn Shr = kInt ? &ShrImpl : nullptr;
    static constexpr PrimBinFn BitAnd = kInt ? &BitAndImpl : nullptr;
    static constexpr PrimBinFn BitOr = kInt ? &BitOrImpl : nullptr;
    static constexpr PrimBinFn BitXor = kInt ? &BitXorImpl : nullptr;
    static constexpr PrimNegFn BitNot = kInt ? &BitNotImpl : nullptr;
};

} // namespace nlang
