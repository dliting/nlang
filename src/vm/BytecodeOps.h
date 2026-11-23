#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace nlang {

//0.13.1a call-site descriptor operand values (OP_CallStatic family).
static constexpr uint16_t kInstDescNone = 0xFFFF;          //no descriptor
static constexpr uint16_t kInstDescForwardCaller = 0xFFFE; //inherit caller frame cell

enum class OpCode : uint8_t {
    // === Control flow ===
    OP_Return,          // return from function
    OP_Jump,            // int16 target_offset
    OP_JumpIfNot,       // int16 target_offset, uint16 local_offset
    OP_Stop,            // halt execution
    OP_AssertFail,      // Phase 9a: uint16 msgStringIdx — throw "assertion failed: <msg>"

    // === Constant load (write inline value to pResult) ===
    OP_ConstInt32,      // int32 value
    OP_ConstFloat,      // float value
    OP_ConstString,     // uint16 pool_index
    OP_ConstZero,       // write zero/nullptr to pResult
    OP_ConstInt64,      // int64 value (8-byte immediate written to pResult)
    OP_ConstDouble,     // double value (8-byte immediate written to pResult)

    // === Variable access (byte-offset into locals) ===
    OP_VarLocal,        // uint16 offset, read locals[offset..] to pResult
    OP_Assign,          // uint16 dst_offset, write pResult to locals[dst_offset..]

    // === Type conversion ===
    OP_PrimCast,        // u8 srcKind, u8 dstKind — convert pResult in place
                        // (kinds are scalar RTK immediates; registry-driven)
    OP_Prim_to_str,     // u8 kind — read the scalar from pResult, format via the
                        // kind's ValueToString, create a string object, write its
                        // handle (int32) to pResult. Covers every scalar→string
                        // coercion (the retired OP_Int32_to_str/OP_Float_to_str
                        // generalized; bool renders true/false, char encodes UTF-8)
    OP_Enum_to_str,     // Phase 8e-9b: uint16 enumDefIdx immediate; read int32 enum value
                        // from pResult, lookup m_compiledModule.enumNames[enumDefIdx][value],
                        // create a string object, write its handle (int32) to pResult
    OP_Array_to_str,    // Phase 9b-pre: no operands; read array heap idx from pResult,
                        // format as "[e1, e2, ...]", create a string object, write handle to pResult

    // === Generalized numeric ops (0.7.5; kind = scalar RTK immediate,
    //     dispatched through registry-instantiated function-pointer tables) ===
    OP_Add,             // u8 kind, u16 dst, u16 src — locals[dst] ⊕= locals[src]
    OP_Sub,
    OP_Mul,
    OP_Div,
    OP_Mod,
    OP_Neg,             // u8 kind, u16 dst — locals[dst] = -locals[dst]
    OP_Cmp,             // u8 kind, u8 cmpOp, u16 lhs, u16 rhs — int32 0/1
                        // written to locals[lhs]; cmpOp ∈ {0 Less, 1 LessEqual,
                        // 2 Greater, 3 GreaterEqual, 4 Equal, 5 NotEqual}

    // === Logical ===
    //&& and || are lowered to short-circuit jump sequences in
    //VmBackend::EmitExpression; OP_LogicalNot is the only eager logical opcode.
    OP_LogicalNot,      // uint16 dst, locals[dst] = !locals[dst]

    // === Function call ===
    OP_CallStatic,        // uint16 func_index, uint16 call_param_base, uint16 descIdx —
                          // receiver-less direct call (static members, free functions);
                          // descIdx fills a generic callee's frame descriptor cell
                          // (0xFFFF none, 0xFFFE forward the caller's own cell)
    OP_ParaEnd,         // mark end of parameter evaluation

    // === Switch/case ===
    OP_Switch,          // uint16 switch_local_offset (the switch value slot)
    OP_Case,            // uint16 jump_to_next (placeholder, patched by clause-exit fixup)
                        // After OP_Case: condition comparison code + OP_JumpIfNot to next case,
                        // then case body code.

    // === String operations ===
    OP_Concat_str,      // uint16 dst, uint16 src — string concatenation
    OP_Eq_str,          // uint16 lhs, uint16 rhs — string equality
    OP_Ne_str,          // uint16 lhs, uint16 rhs — string inequality
    OP_Less_str,        // uint16 lhs, uint16 rhs — bytewise relational compare (Phase 11 Q4);
    OP_LessEqual_str,   //   UTF-8 byte order == code point order, so this is also
    OP_Greater_str,     //   code point order. null operand compares as pool[0].
    OP_GreaterEqual_str,
    OP_StrLen,          // uint16 dst, uint16 src — string length → int32

    // === Debug ===
    OP_DebugInfo,       // uint16 info

    // === Struct operations ===
    OP_AllocStruct,     // uint16 dst, uint16 structIdx, uint16 fieldCount
    OP_LoadField,       // uint16 dst, uint16 obj, uint16 fieldOff
    OP_StoreField,      // uint16 obj, uint16 fieldOff, uint16 src
    OP_CopyStruct,      // uint16 dst, uint16 src, uint16 structIdx

    // === Class operations ===
    OP_New,             // uint16 dst, uint16 classIdx, uint16 descIdx (0xFFFF = none) — allocate object on heap
    OP_CallMethod,      // uint16 methodNameStringIdx, uint16 callParamBase — virtual method dispatch (name-based)
    OP_CallMethodDirect,// uint16 funcIdx, uint16 callParamBase — non-virtual method call
    OP_CallIntrinsic,   // uint16 intrinsicId, uint16 callParamBase — intrinsic function call
    OP_NullCheck,       // uint16 obj — throw if locals[obj] is null/invalid (hard crash, like Java NPE)

    // === Array operations ===
    OP_AllocArrayN,     // uint16 dst, uint16 typeIdx, uint16 rank, uint16 dimsBase,
                        // uint16 descIdx (0xFFFF = none) — v0.13.1: the element
                        // instantiation's descriptor row when the element type is
                        // a generic instantiation; stamped as the record's element
                        // descriptor so array casts can tell Box<int>[] and
                        // Box<string>[] apart (one arrayTypes row per erasure)
    OP_LoadElementN,    // uint16 dst, uint16 arr, uint16 idxBase, uint16 rank
    OP_StoreElementN,   // uint16 arr, uint16 idxBase, uint16 src, uint16 rank
    OP_AssembleArrayN,  // uint16 dst, uint16 typeIdx, uint16 elemsBase, uint16 count,
                        // uint16 descIdx (0xFFFF = none) — v0.13.1: the literal-
                        // array twin stamps the element descriptor like the
                        // allocator above
    OP_LoadElementFlat, // uint16 dst, uint16 arr, uint16 flatIdx
    OP_ArrayLength,     // uint16 dst, uint16 arr (elemCount, any rank)
    OP_ArrayShape,      // uint16 dst (int[rank]), uint16 arr
    OP_MakeArrayView,   // uint16 dst, uint16 arr, uint16 typeIdx, uint16 rank, uint16 paramsBase — slice subscript; params = 3×rank int32 slots (start/stop/step per axis, INT32_MIN default sentinel) followed by ceil(4×rank/32) mask slots (4 bits/axis: start/stop/step present + axis-is-scalar)

    // === Phase 8e-1: Object protocol + boxing/cast ===
    OP_Box,             // uint8 typeKind — pop primitive, push boxed Object ref
    OP_Unbox,           // uint8 typeKind — pop Object ref, type-check, push primitive
    OP_CheckCast,       // uint16 classIdx, uint16 descIdx (0xFFFF = none) — verify
                        // subclass; when descIdx names a descriptor row the
                        // operand's instantiation spelling must match too
                        // (v0.13.1: same template row, different type args)

    // === Phase 9d: Exception handling ===
    OP_Throw,           // uint16 src — throw Exception at locals[src] (heap idx)
    OP_Rethrow,         // no operands — rethrow current catch's exception
    OP_PopHandler,      // no operands — pop one entry from handlerExcStack at catch exit

    // === Phase 13: first-class function values (func<...>) ===
    OP_MakeFunc,        // uint16 funcIdx, uint16 descIdx — allocate a static
                        // function handle {funcIdx, this=0, form=0, descIdx},
                        // write heap idx to pResult; descIdx is the
                        // instantiation descriptor row (kInstDescNone when the
                        // target is non-generic)
    OP_CallDelegate,    // uint16 calleeLocal, uint16 callParamBase — invoke the
                        // handle at locals[calleeLocal]; this==0 → free function,
                        // params staged at callParamBase[0..n)
    OP_Eq_func,         // uint16 lhs, uint16 rhs — content equality (null-guarded)
    OP_Ne_func,         // uint16 lhs, uint16 rhs — negation of OP_Eq_func
    //v0.13.0: struct ==/!= by field value (the DeepCopyStruct dual) —
    //the same equality the container entries use, so contains(p) and
    //get(i) == p agree. Operands are valid struct handles (the resolver
    //gate allows same-type struct pairs only); null sentinels compare
    //as plain indices, defensively.
    OP_Eq_struct,       // uint16 lhs, uint16 rhs — field-value equality
    OP_Ne_struct,       // uint16 lhs, uint16 rhs — negation of OP_Eq_struct
    OP_Func_to_str,     // no operands; format the handle at pResult as
                        // "func <name>", create a string object, write handle to pResult
    OP_MakeBoundFunc,   // uint16 funcIdx — receiver at the pResult slot →
                        // {funcIdx, this, form=0}; null receiver throws at bind
    OP_MakeVFunc,       // uint16 nameIdx (string-constant index) — receiver at the
                        // pResult slot → {nameIdx, this, form=1}; dispatch
                        // resolves the method by name on the runtime class

    // === 0.7.5 char/string bridge ===
    OP_StrByteAt,       // uint16 dst, uint16 str, uint16 idx — read the idx-th
                        // BYTE of the string into locals[dst] as ubyte (byte
                        // semantics, like OP_StrLen/substring/indexOf); out of
                        // range raises IndexOutOfBoundsException
    OP_StrForeachStep,  // uint16 str, uint16 off, uint16 cond, uint16 ch — one
                        // code-point iteration step: decode the UTF-8 code
                        // point at byte offset locals[off] into locals[ch],
                        // advance off by its UTF-8 length, write 1/0 continue
                        // flag to locals[cond] (0 = off at/past byte length)

    // === 0.9.9 shift operators ===
    OP_Shl,             // u8 kind, u16 dst, u16 src — locals[dst] <<= count;
                        // count must lie in [0, 8*sizeof(T)) or a named error
    OP_Shr,             // u8 kind, u16 dst, u16 src — same, arithmetic on
                        // signed rows and logical on unsigned rows

    // === 0.13.7 bitwise operators (integer carrier rows only) ===
    OP_BitAnd,          // u8 kind, u16 dst, u16 src — locals[dst] &= locals[src]
    OP_BitOr,           // u8 kind, u16 dst, u16 src — locals[dst] |= locals[src]
    OP_BitXor,          // u8 kind, u16 dst, u16 src — locals[dst] ^= locals[src]
    OP_BitNot,          // u8 kind, u16 dst — locals[dst] = ~locals[dst]

    // === 0.12.6: static slots (module-wide table, spec §6.2) ===
    OP_LoadStatic,      // uint16 dst, uint16 slotIdx — frame[dst] = slots[slotIdx]
    OP_StoreStatic,     // uint16 slotIdx, uint16 src — slots[slotIdx] = frame[src]

    // === 0.12.6: lazy static initialization (spec §6.2 D2/D4/D5/D10) ===
    OP_ClassInitGate,   // u16 classIdx, u16 donePc (absolute), u16 kind
                        // (0 = class, 1 = interface) — Done jumps donePc;
                        // the first claimer falls through to the paired
                        // OP_CallStatic of the owner's $staticInit; a later
                        // context parks and re-walks the gate on wake
    OP_ClassInitDone,   // u16 classIdx, u16 kind — publish Done, wake waiters
    OP_ClassInitFail,   // u16 classIdx, u16 kind — reset the owner so the
                        // next use retries (D10); pair with OP_Rethrow

    // === 0.12.6 Task 7: static-local slot machines (spec §8) ===
    OP_SlotInitGate,    // u16 slotIdx, u16 donePc (absolute) — Done and
                        // re-entrant-self jump donePc; the first claimer
                        // falls through to the guarded initializer; anyone
                        // else parks and re-walks the gate on wake
    OP_SlotInitDone,    // u16 slotIdx — publish Done, wake waiters
    OP_SlotInitFail,    // u16 slotIdx — reset the machine so the next use
                        // retries (D10); pair with OP_Rethrow

    // === v0.12.7 Task 3: boundary rank check ===
    OP_CheckArrayRank,  // u16 dst, u16 src, u16 rank — null passes through
                        // unchanged; otherwise the RTK_Array record's rank
                        // cell must equal rank or a catchable
                        // InvalidCastException is raised

    // === v0.12.7 Task 5: Object→array downcast check ===
    OP_CheckArrayType,  // u16 dst, u16 src, u16 arrayTypeIdx, u16 descIdx — null
                        // passes through; the record's type identity (kind +
                        // arrayTypes row) must match or a catchable
                        // InvalidCastException is raised. v0.13.1: a named
                        // element-descriptor row must also match the record's
                        // stamped element descriptor (instantiation elements)

    // === v0.12.7 Task 4: unannotated Array<E> spelling (no static rank) ===
    OP_LoadElementDyn,  // u16 dst, u16 arr, u16 idxBase, u16 nCoords — the
                        // coordinate count verifies against the record's
                        // rank cell; mismatch raises catchable
                        // IndexOutOfBoundsException (both counts)
    OP_StoreElementDyn, // u16 arr, u16 idxBase, u16 src, u16 nCoords —
                        // store twin of OP_LoadElementDyn
    OP_AllocArrayDyn,   // u16 dst, u16 typeIdx, u16 dimsSlot, u16 descIdx
                        // (0xFFFF = none) — dims is a
                        // rank-1 int[] handle; the rank = dims.length()
                        // validates in [1, kMaxArrayRank] and the extents
                        // go through AllocArrayOnHeap. v0.13.1: descIdx is
                        // the element instantiation's descriptor row (the
                        // Array(dims) allocation stamps it like every
                        // other array creation form)

    // === v0.13.1: runtime generic type info (typeName) ===
    OP_TypeNameArg,     // u16 dst, u8 paramOrdinal — read the receiver's
                        // instantiation descriptor (local 0), take the
                        // ordinal-th type argument's canonical name,
                        // create a string object, write its handle to
                        // locals[dst]
    OP_TypeNameCompose, // u16 dst, u16 compositionIdx — compose the composition's
                        // spelling against the receiver's instantiation
                        // (ARG_THIS steps read local 0), intern the
                        // descriptor, create a string of its canonical
                        // spelling, write the handle to locals[dst]

    // === v0.13.1 D6: kind-dispatched T operations (==/!= and to-string) ===
    OP_Eq_T,            // u16 dst, u16 paramOrdinal, u16 lhs, u16 rhs —
                        // compare two T-typed slots under the receiver's
                        // ordinal-th type argument kind (scalar inline
                        // cell rule, string content, struct field value,
                        // class/array identity, func record content);
                        // 0xFFFF marks the null-literal side (no slot:
                        // the reference null test of the kind, and a
                        // scalar kind is never null); result bool to dst
    OP_Ne_T,            // same operands, negated verdict
    OP_TypeArg_to_str,  // u8 paramOrdinal — convert the T-typed value in
                        // the accumulator through the kind's own render
                        // (scalar registry, string identity, FormatHeap
                        // Value walk) and write the string handle back
    OP_As_TypeParam,    // u8 paramOrdinal — `x as T`: decide the cast by
                        // the receiver's ordinal-th type argument kind
                        // (boxed scalar unbox, string kind check, struct
                        // row identity, class super-chain + instantiation
                        // spelling, array element key); a mismatch raises
                        // catchable InvalidCastException, null passes
                        // through; the accumulator holds the value in and
                        // out (reference) or the unboxed payload out

    // === 0.13.1a Task 5: descriptor-dispatched container-boundary
    // normalization (the uniform-T-representation invariant: in a
    // generic frame a T-typed value is raw iff T's kind is scalar,
    // while erased List/Dict/Channel store every element as a heap
    // handle). Emitted where the static boxing plan cannot resolve
    // the tag — the receiver's type argument is the calling generic
    // body's own placeholder; the executing frame's descriptor decides.
    OP_Box_TypeArg,     // u8 paramOrdinal — normalize the accumulator's
                        // T-typed value INTO the erased-container element
                        // form: scalar kinds box under the descriptor's
                        // own tag (OpBox mechanics), reference kinds pass
                        // through (the handle IS the stored form)
    OP_Unbox_TypeArg,   // u8 paramOrdinal — the inverse boundary: an
                        // erased-container element INTO the T-typed frame
                        // form: scalar kinds unbox under the descriptor's
                        // tag (OpUnbox mechanics), reference kinds pass
                        // through as the handle

    //Record stabilization (v0.13.9 phase ②): overwrite the dst record's
    //cells in place from src — zero allocation on the established path.
    //A null dst handle (≤ 0) establishes one fresh record instead. Same
    //operand layout as OP_CopyStruct; appended at the table end (the
    //enum is append-only — mid-table rows would renumber every later
    //opcode and break every stored image).
    OP_CopyStructInPlace,  // uint16 dst, uint16 src, uint16 structIdx

    // === v0.13.9 phase ③: the byref slot representation ===
    //A reference value is one uniform 8-byte frame slot holding the pair
    //(base heap-record handle, cell offset within that record) — design
    //doc §3.2. The RefOf* family builds the pair for every bindable
    //location; OP_RefHandle extracts the base handle back into a
    //struct-valued slot (byrefs are never stored, so consumption always
    //goes through the handle); OP_LoadRef/OP_StoreRef dereference the
    //pair. The pair is a raw slot value: moving it between slots is the
    //existing 8-byte raw copy (OP_VarLocal/OP_Assign), zero allocation.
    OP_RefOfLocal,      // uint16 dst, uint16 src — src is a struct-record
                        // slot; dst = (handle, 0)
    OP_RefOfPromoted,   // uint16 dst, uint16 src — src low half is a boxed
                        // record handle (an &-bound scalar/string local's
                        // stable boxing record); dst = (handle, 1)
    OP_RefOfField,      // uint16 dst, uint16 base, uint16 cellOff — dst =
                        // (base handle, cellOff); cellOff is in HEAP CELLS
                        // (int32 units), not bytes — byref dereference walks
                        // cells directly, field ops address 8-byte rows
    OP_RefOfField_T,    // uint8 paramOrdinal, uint16 dst, uint16 base,
                        // uint16 cellOff — the generic-body field bind: the
                        // frame context's type argument kind picks the pair
                        // shape — scalar/string pair the field's cell like
                        // OP_RefOfField, struct/identity deref the cell and
                        // pair the record it names (offset 0)
    OP_RefOfElement,    // uint16 dst, uint16 arr, uint16 flatSlot — null
                        // check + bounds check + view affine like
                        // OP_LoadElementFlat; dst = (arr handle, payload
                        // base + flat*elemCells)

    OP_RefOfElementN,   // uint16 dst, arr, idxBase, rank — rank-N
                        // spelling of RefOfElement; the runtime computes
                        // the flat address from the staged indices
    OP_RefHandle,       // uint16 dst, uint16 src — dst low half = the
                        // byref pair's base handle (struct& consumption)
    OP_LoadRef,         // uint8 kind, uint16 dst, uint16 src — read the
                        // cells the src pair addresses into dst (8-byte
                        // slot fill; scalar width by kind, strings one cell)
    OP_LoadRef_T,       // uint8 paramOrdinal, uint16 dst, uint16 src —
                        // the generic-body read through a T& formal pair:
                        // the frame context's ordinal-th type argument
                        // kind dispatches — scalar/string deref like
                        // OP_LoadRef, struct/identity copy the pair raw
                        // (offset-0 pairs read as record handles, D9)
    OP_StoreRef,        // uint8 kind, uint16 dstByref, uint16 srcVal —
                        // write the cells the dstByref pair addresses
                        // from srcVal
    OP_StoreRef_T,      // uint8 paramOrdinal, uint16 dstByref,
                        // uint16 srcVal — the generic-body store through
                        // a T& formal pair: the frame context's
                        // ordinal-th type argument kind dispatches —
                        // scalar/string take the OP_StoreRef cell write,
                        // struct overwrites the pair's base record in
                        // place, identity kinds rebind the slot alone
                        // (T&≡T, design D9)

    OP_Count
};


const char* OpCodeName(OpCode op);

} // namespace nlang
