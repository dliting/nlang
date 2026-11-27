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
//v0.13.9 phase ③: declared kind of a byref slot (an &-bound local,
//parameter or return slot). The slot holds the pair (base heap-record
//handle, cell offset) — design doc §3.2 — so the base handle is always a
//heap-domain record (struct/boxed/array) and the GC classifies the slot
//as ESC_HeapIndex (single domain, like a struct slot's low half). Byrefs
//are never stored: no field, element, container entry or static slot ever
//declares this kind — it exists only on frame slot descriptors (and the
//return-kind cell), which is why MarkFrameLocals/MarkFrameBand style
//consumers route it and record-side consumers never see it.
static constexpr uint8_t RTK_Byref = 22;
//v0.13.9 phase ③ Task 11: declared kind of a PROMOTED slot (a scalar or
//string local/param whose storage became a stable boxed record because a
//& binding aliases it). Only the slot's low half is meaningful — the box
//handle; the upper half is stale bytes the prologue's Assign left (every
//consumer reads the low half). The GC routes it exactly like RTK_Byref
//(ESC_HeapIndex, over-mark before the prologue rewrites a raw argument),
//and like RTK_Byref it never appears on fields, elements, container
//entries, static slots or return kinds. The debug display is the reason
//the two kinds split: a promoted slot renders its plain value, a byref
//slot renders the base->cell->value pair (ndb, spec §4). Kind-value
//extension of the locals-table byte — rides the v2.21 floor, no layout
//change.
static constexpr uint8_t RTK_Promoted = 23;
//v0.13.9 phase ③ Task 11: declared kind of a CELL-addressed byref slot
//(`int&`/`string&`/enum-`&` bindings — a reference whose declared element
//type is a scalar, string or enum). The pair always names a payload CELL
//(box payload, struct/class field cell, array element cell), never a
//whole record, so the debug display resolves the element kind from the
//base family's own metadata even at cell 0 — the aggregate family
//(RTK_Byref) renders an offset-0 pair as the whole record per the D9
//convention, which is the wrong read for a first-field scalar &. Same
//GC routing as RTK_Byref (ESC_HeapIndex on the pair's base handle).
//Kind-value extension of the locals-table byte — rides the v2.21 floor,
//no layout change.
static constexpr uint8_t RTK_ByrefCell = 24;
//v0.13.9 phase ③ Task 12: the IDENTITY-tier kind of an interface type —
//TypeDesc channels (static-slot/param/return descriptors, field
//descriptors) and arrayTypes element rows. It names WHICH interface the
//declaration carries: the type-index/elem-index cell holds the
//interfaces-table row, so cast rows and linker merges keep interface
//identity (a shared sentinel row would let `I[] as J[]` pass; the old
//scalar degrade also collided with int[] rows). The STORAGE tier is a
//different question with a different answer: a slot or field typed by an
//interface HOLDS a class handle, and RuntimeTypeKind files it RTK_Class —
//RTK_Interface never appears in locals-table bytes, fieldTypeKinds,
//signature leaves or return kinds. GC routing: a value under this kind
//is a heap reference exactly like RTK_Class (MarkStaticSlotRoots's
//reference arm; ArrayElemKindAccepts accepts runtime RTK_Class records).
static constexpr uint8_t RTK_Interface = 25;
//A display field storing a captured & binding holds the (base, cell)
//pair copied from a frame slot. Unlike RTK_Byref and RTK_ByrefCell, this
//kind occurs in class field descriptors. Only its base is a heap edge;
//the offset is never a handle. Since the base can name different record
//families, the class mark phase scans both domains conservatively.
static constexpr uint8_t RTK_ByrefBase = 26;
//Descriptor-only kinds RTK_List / RTK_Dict / RTK_NonSerialized live in
//TypeDesc.h (they appear inside TypeDesc, never in these legacy bytes).
static constexpr uint8_t RTK_Null   = 0xFD;  //Option B: null default value (any reference type)
static constexpr uint8_t RTK_Unfoldable = 0xFC;  //Option B: had default but not constant-foldable
static constexpr uint8_t RTK_Expr   = 0xFB;  //v2.3: default is the ExprDesc tree in exprBytes
static constexpr uint8_t RTK_Void   = 0xFE;  //used for ctor/void method stubs

} //namespace nlang
