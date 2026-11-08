#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace nlang {

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
                        // kind's ValueToString, mint a string object, write its
                        // handle (int32) to pResult. Covers every scalar→string
                        // coercion (the retired OP_Int32_to_str/OP_Float_to_str
                        // generalized; bool renders true/false, char encodes UTF-8)
    OP_Enum_to_str,     // Phase 8e-9b: uint16 enumDefIdx immediate; read int32 enum value
                        // from pResult, lookup m_compiledModule.enumNames[enumDefIdx][value],
                        // mint a string object, write its handle (int32) to pResult
    OP_Array_to_str,    // Phase 9b-pre: no operands; read array heap idx from pResult,
                        // format as "[e1, e2, ...]", mint a string object, write handle to pResult

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
    OP_CallFunc,        // uint16 func_index, uint16 call_param_base; call function, result to pResult
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
    OP_AllocArrayN,     // uint16 dst, uint16 typeIdx, uint16 rank, uint16 dimsBase
    OP_LoadElementN,    // uint16 dst, uint16 arr, uint16 idxBase, uint16 rank
    OP_StoreElementN,   // uint16 arr, uint16 idxBase, uint16 src, uint16 rank
    OP_AssembleArrayN,  // uint16 dst, uint16 typeIdx, uint16 elemsBase, uint16 count
    OP_LoadElementFlat, // uint16 dst, uint16 arr, uint16 flatIdx
    OP_ArrayLength,     // uint16 dst, uint16 arr (elemCount, any rank)
    OP_ArrayShape,      // uint16 dst (int[rank]), uint16 arr
    OP_MakeArrayView,   // uint16 dst, uint16 arr, uint16 typeIdx, uint16 rank, uint16 paramsBase — slice subscript; params = 3×rank int32 slots (start/stop/step per axis, INT32_MIN default sentinel) followed by ceil(4×rank/32) mask slots (4 bits/axis: start/stop/step present + axis-is-scalar)

    // === Phase 8e-1: Object protocol + boxing/cast ===
    OP_Box,             // uint8 typeKind — pop primitive, push boxed Object ref
    OP_Unbox,           // uint8 typeKind — pop Object ref, type-check, push primitive
    OP_CheckCast,       // uint16 classIdx — pop ref, verify subclass, throw on mismatch

    // === Phase 9d: Exception handling ===
    OP_Throw,           // uint16 src — throw Exception at locals[src] (heap idx)
    OP_Rethrow,         // no operands — rethrow current catch's exception
    OP_PopHandler,      // no operands — pop one entry from handlerExcStack at catch exit

    // === Phase 9e: out-parameter calls ===
    OP_CallFuncOut,         // uint16 funcIdx, uint16 callParamBase, uint32 outMask —
                            // like OP_CallFunc; after the callee returns, bit i of outMask
                            // copies callee frame slot i back to caller staging slot i
    OP_CallMethodDirectOut, // uint16 funcIdx, uint16 callParamBase, uint32 outMask —
                            // same for non-virtual method calls (this = slot 0)

    // === Phase 13: first-class function values (func<...>) ===
    OP_MakeFunc,        // uint16 funcIdx — allocate a static function handle
                        // {funcIdx, this=0, form=0}, write heap idx to pResult
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
                        // "func <name>", mint a string object, write handle to pResult
    OP_MakeBoundFunc,   // uint16 funcIdx — receiver at the pResult slot →
                        // {funcIdx, this, form=0}; null receiver throws at bind
    OP_MakeVFunc,       // uint16 nameIdx (string-constant index) — receiver at the
                        // pResult slot → {nameIdx, this, form=1}; dispatch
                        // resolves the method by name on the runtime class
    OP_CallDelegateOut, // uint16 calleeLocal, uint16 callParamBase, uint32
                        // outMask — OP_CallDelegate + out write-back; bound
                        // handles reverse the this-shift when copying back

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

    // === 0.12.6: static slots (module-wide table, spec §6.2) ===
    OP_LoadStatic,      // uint16 dst, uint16 slotIdx — frame[dst] = slots[slotIdx]
    OP_StoreStatic,     // uint16 slotIdx, uint16 src — slots[slotIdx] = frame[src]

    // === 0.12.6: lazy static initialization (spec §6.2 D2/D4/D5/D10) ===
    OP_ClassInitGate,   // u16 classIdx, u16 donePc (absolute), u16 kind
                        // (0 = class, 1 = interface) — Done jumps donePc;
                        // the first claimer falls through to the paired
                        // OP_CallFunc of the owner's $staticInit; a later
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
    OP_CheckArrayType,  // u16 dst, u16 src, u16 arrayTypeIdx — null passes
                        // through; the record's type identity (kind +
                        // arrayTypes row) must match or a catchable
                        // InvalidCastException is raised

    // === v0.12.7 Task 4: unannotated Array<E> spelling (no static rank) ===
    OP_LoadElementDyn,  // u16 dst, u16 arr, u16 idxBase, u16 nCoords — the
                        // coordinate count verifies against the record's
                        // rank cell; mismatch raises catchable
                        // IndexOutOfBoundsException (both counts)
    OP_StoreElementDyn, // u16 arr, u16 idxBase, u16 src, u16 nCoords —
                        // store twin of OP_LoadElementDyn
    OP_AllocArrayDyn,   // u16 dst, u16 typeIdx, u16 dimsSlot — dims is a
                        // rank-1 int[] handle; the rank = dims.length()
                        // validates in [1, kMaxArrayRank] and the extents
                        // go through AllocArrayOnHeap

    // === v0.13.1: runtime generic type info (typeName) ===
    OP_TypeNameArg,     // u16 dst, u8 paramOrdinal — read the receiver's
                        // instantiation descriptor (local 0), take the
                        // ordinal-th type argument's canonical spelling,
                        // mint a string object, write its handle to
                        // locals[dst]
    OP_TypeNameCompose, // u16 dst, u16 recipeIdx — compose the recipe's
                        // spelling against the receiver's instantiation
                        // (ARG_THIS steps read local 0), intern the
                        // descriptor, mint a string of its canonical
                        // spelling, write the handle to locals[dst]

    OP_Count
};

//Single source of truth for the byte extent of each instruction: the
//disassembler walk (line/pc map, ndb), the ndisasm printer and the
//.ncu linker remap all step this one table. Parallel per-consumer
//stride tables drifted more than once (AllocArrayN linker mispatch,
//ND element opcodes walking stale group strides — both surfaced as
//"corrupt image" far from the cause), so the extent lives here, next
//to the operand shapes documented in the enum. Keep in lockstep with
//the emitters when adding or reshaping opcodes; an unlisted opcode is
//an authoring bug and fails loudly instead of corrupting downstream.
//Extent groups for InstructionStride: opcodes sharing one instruction
//byte extent; the width expressions mirror the operand shapes
//documented in the enum (1 opcode byte + operands). Plain arrays plus
//the StrideGroup helper (auto-carried count) — a constexpr aggregate
//holding std::initializer_list members silently failed to match its
//inline-braced rows under MSVC (op=5 walked straight to the miss),
//so the table stores pointers and counts instead.
static constexpr OpCode kStrideNoOperands[] = {
    OpCode::OP_Return, OpCode::OP_Stop, OpCode::OP_ConstZero,
    OpCode::OP_Array_to_str, OpCode::OP_Func_to_str,
    OpCode::OP_ParaEnd, OpCode::OP_Rethrow, OpCode::OP_PopHandler};
static constexpr OpCode kStrideByteTag[] = {
    OpCode::OP_Prim_to_str, OpCode::OP_Box, OpCode::OP_Unbox};
static constexpr OpCode kStrideOneWord[] = {
    OpCode::OP_Jump, OpCode::OP_Case, OpCode::OP_ConstString,
    OpCode::OP_AssertFail, OpCode::OP_VarLocal, OpCode::OP_Assign,
    OpCode::OP_Enum_to_str, OpCode::OP_LogicalNot, OpCode::OP_Switch,
    OpCode::OP_DebugInfo, OpCode::OP_NullCheck, OpCode::OP_CheckCast,
    OpCode::OP_Throw, OpCode::OP_MakeFunc, OpCode::OP_MakeBoundFunc,
    OpCode::OP_MakeVFunc,
    OpCode::OP_SlotInitDone, OpCode::OP_SlotInitFail};
static constexpr OpCode kStrideKindWord[] = {
    //v0.13.1: OP_TypeNameArg is u16 dst + u8 ordinal — word-then-tag
    //(OP_Neg is tag-then-word); the group encodes only the 4-byte extent.
    OpCode::OP_Neg, OpCode::OP_TypeNameArg};
static constexpr OpCode kStrideKindByte[] = {
    OpCode::OP_PrimCast};
static constexpr OpCode kStrideTwoWords[] = {
    OpCode::OP_JumpIfNot, OpCode::OP_Concat_str, OpCode::OP_Eq_str,
    OpCode::OP_Ne_str, OpCode::OP_Less_str, OpCode::OP_LessEqual_str,
    OpCode::OP_Greater_str, OpCode::OP_GreaterEqual_str,
    OpCode::OP_StrLen, OpCode::OP_CallFunc, OpCode::OP_CallMethod,
    OpCode::OP_CallMethodDirect, OpCode::OP_CallIntrinsic,
    OpCode::OP_CallDelegate, OpCode::OP_Eq_func, OpCode::OP_Ne_func,
    OpCode::OP_Eq_struct, OpCode::OP_Ne_struct,
    OpCode::OP_ArrayLength, OpCode::OP_ArrayShape,
    OpCode::OP_LoadStatic, OpCode::OP_StoreStatic,
    OpCode::OP_ClassInitDone, OpCode::OP_ClassInitFail,
    OpCode::OP_SlotInitGate, OpCode::OP_TypeNameCompose};
static constexpr OpCode kStrideKindTwoWords[] = {
    OpCode::OP_Add, OpCode::OP_Sub, OpCode::OP_Mul, OpCode::OP_Div,
    OpCode::OP_Mod, OpCode::OP_Shl, OpCode::OP_Shr};
static constexpr OpCode kStrideThreeWords[] = {
    //v0.13.1: OP_New gained its third u16 (descIdx) — dst, classIdx,
    //descIdx, the same extent family as AllocStruct's three words.
    OpCode::OP_New,
    OpCode::OP_AllocStruct, OpCode::OP_LoadField,
    OpCode::OP_StoreField, OpCode::OP_CopyStruct,
    OpCode::OP_LoadElementFlat, OpCode::OP_StrByteAt,
    OpCode::OP_ClassInitGate, OpCode::OP_CheckArrayRank,
    OpCode::OP_CheckArrayType, OpCode::OP_AllocArrayDyn};
static constexpr OpCode kStrideFourWords[] = {
    OpCode::OP_LoadElementN, OpCode::OP_StoreElementN,
    OpCode::OP_LoadElementDyn, OpCode::OP_StoreElementDyn,
    OpCode::OP_AssembleArrayN,
    OpCode::OP_AllocArrayN, OpCode::OP_StrForeachStep};
static constexpr OpCode kStrideFiveWords[] = {
    OpCode::OP_MakeArrayView};
static constexpr OpCode kStrideOutMaskWords[] = {
    OpCode::OP_CallFuncOut, OpCode::OP_CallMethodDirectOut,
    OpCode::OP_CallDelegateOut};
static constexpr OpCode kStrideCmpHead[] = {
    OpCode::OP_Cmp};
static constexpr OpCode kStrideConst32[] = {
    OpCode::OP_ConstInt32, OpCode::OP_ConstFloat};
static constexpr OpCode kStrideConst64[] = {
    OpCode::OP_ConstInt64, OpCode::OP_ConstDouble};

struct StrideExtentGroup {
    const OpCode* ops;
    size_t count;
    size_t stride;
};

//Count is carried from the array declaration itself — a group row can
//never drift from its member list.
template <size_t N>
constexpr StrideExtentGroup StrideGroup(const OpCode (&ops)[N],
                                        size_t stride) {
    return {ops, N, stride};
}

//Single source of truth for the byte extent of each instruction: the
//disassembler walk (line/pc map, ndb), the ndisasm printer and the
//.ncu linker remap all step this one table. Parallel per-consumer
//stride tables drifted more than once (AllocArrayN linker mispatch,
//ND element opcodes walking stale group strides — both surfaced as
//"corrupt image" far from the cause), so the extent lives here, next
//to the operand shapes documented in the enum. Keep in lockstep with
//the emitters when adding or reshaping opcodes; an unlisted opcode is
//an authoring bug and fails loudly instead of corrupting downstream.
inline size_t InstructionStride(OpCode op) {
    static constexpr StrideExtentGroup kStrideGroups[] = {
        StrideGroup(kStrideNoOperands, 1),
        StrideGroup(kStrideByteTag, 1 + 1),
        StrideGroup(kStrideOneWord, 1 + 2),
        StrideGroup(kStrideKindWord, 1 + 1 + 2),
        StrideGroup(kStrideKindByte, 1 + 1 + 1),
        StrideGroup(kStrideTwoWords, 1 + 2 + 2),
        StrideGroup(kStrideKindTwoWords, 1 + 1 + 2 + 2),
        StrideGroup(kStrideThreeWords, 1 + 2 + 2 + 2),
        StrideGroup(kStrideFourWords, 1 + 2 + 2 + 2 + 2),
        StrideGroup(kStrideFiveWords, 1 + 2 + 2 + 2 + 2 + 2),
        StrideGroup(kStrideOutMaskWords, 1 + 2 + 2 + 4),
        StrideGroup(kStrideCmpHead, 1 + 1 + 1 + 2 + 2),
        StrideGroup(kStrideConst32, 1 + 4),
        StrideGroup(kStrideConst64, 1 + 8)};
    for (const StrideExtentGroup& group : kStrideGroups)
        for (size_t i = 0; i < group.count; ++i)
            if (group.ops[i] == op)
                return group.stride;
    throw std::runtime_error("unknown opcode in disassembler");
}

const char* OpCodeName(OpCode op);

} // namespace nlang
