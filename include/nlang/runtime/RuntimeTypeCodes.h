/*---
    RuntimeTypeCodes.h — the wire-visible type kind codes (public header).

    The RTK_* numbers name a type kind wherever a type crosses a boundary: in
    .ncu/.npkg records (local descriptors, field kinds, signatures, default
    value tags) and in heap record headers. The producer (compiler backend)
    and the consumer (VM loader and executor) must agree on them, so the codes
    live in the runtime layer — the one layer both of those libraries may
    include. Keeping them in the VM header instead forced the compiler's public
    spelling header to include a VM header, which is the wrong direction for
    the dependency (the VM consumes the compiler's AST, not the other way
    round).

    The scalar block (RTK_Int32/RTK_Float = 0/1 and RTK_Byte..RTK_Char =
    10..19) is derived from the scalar primitive registry and stays in
    PrimitiveTypes.h — single source per block. The descriptor-only kinds
    RTK_List / RTK_Dict / RTK_NonSerialized appear inside a type descriptor
    only, never in these bytes, and stay in TypeDesc.h.
---*/
#pragma once
#include <cstdint>

namespace nlang {

//Runtime type kind constants for serialization (compile-time NK_* values
//exceed uint8_t range). The scalar codes (RTK_Int32/RTK_Float = 0/1 plus
//RTK_Byte..RTK_Char = 10..19) live in PrimitiveTypes.h — single source.
static constexpr uint8_t RTK_String = 2;
static constexpr uint8_t RTK_Struct = 3;
static constexpr uint8_t RTK_Class  = 4;
static constexpr uint8_t RTK_Array  = 5;
static constexpr uint8_t RTK_Boxed  = 6;  //boxed primitive (slot[0]=tag, slots[1..2]=value; 8-byte family uses both cells)
static constexpr uint8_t RTK_Func   = 7;  //Phase 13: function handle (slot[0]=target, slot[1]=this, slot[2]=form; 0=static, 1=virtual)
//v0.12.3: declared kind of a type-parameter (T) slot — the template row's
//one-implementation layout. A T slot holds one value slot whatever the
//instantiation binds, and its runtime representation depends on the type
//argument (string store handle / heap index / inline scalar), so every
//consumer routes it conservatively (GC tests both domains). First kind
//above the scalar block (10..19), keeping the two ranges disjoint.
static constexpr uint8_t RTK_TypeParam = 20;
//v0.12.7: runtime-only kind of an array VIEW record (spec §4.2 of the
//array/tensor design). Never a declared type kind — it never appears on
//the wire or in locals tables; it labels heap records whose header
//prefix mirrors RTK_Array (kind/typeIdx/rank/elemCount/dims at the same
//offsets) and whose tail carries strides/offset/target. The target cell
//always names an OWNED RTK_Array record (depth one).
static constexpr uint8_t RTK_ArrayView = 21;
//Descriptor-only kinds RTK_List / RTK_Dict / RTK_NonSerialized live in
//TypeDesc.h (they appear inside TypeDesc, never in these legacy bytes).
static constexpr uint8_t RTK_Null   = 0xFD;  //Option B: null default value (any reference type)
static constexpr uint8_t RTK_Unfoldable = 0xFC;  //Option B: had default but not constant-foldable
static constexpr uint8_t RTK_Expr   = 0xFB;  //v2.3: default is the ExprDesc tree in exprBytes
static constexpr uint8_t RTK_Void   = 0xFE;  //used for ctor/void method stubs

} //namespace nlang
