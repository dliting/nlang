/*---
    BytecodeStrides.h — the instruction-extent tables split from
    BytecodeOps.h (source-size guard, EmitStmtRef.cpp precedent). The
    opcode enum stays in BytecodeOps.h; the three stride consumers
    (the disassembler walk, the ndisasm printer, the .ncu linker
    remap) include this header for InstructionStride.
---*/
#pragma once
#include "BytecodeOps.h"
#include <cstddef>
#include <stdexcept>

namespace nlang
{

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
    OpCode::OP_Prim_to_str, OpCode::OP_Box, OpCode::OP_Unbox,
    //v0.13.1 D6: the T to-string conversion and the as-T cast — one
    //ordinal byte each.
    OpCode::OP_TypeArg_to_str, OpCode::OP_As_TypeParam,
    //0.13.1a Task 5: the container-boundary normalization pair — one
    //ordinal byte each.
    OpCode::OP_Box_TypeArg, OpCode::OP_Unbox_TypeArg};
static constexpr OpCode kStrideOneWord[] = {
    OpCode::OP_Jump, OpCode::OP_Case, OpCode::OP_ConstString,
    OpCode::OP_AssertFail, OpCode::OP_VarLocal, OpCode::OP_Assign,
    OpCode::OP_Enum_to_str, OpCode::OP_LogicalNot, OpCode::OP_Switch,
    OpCode::OP_DebugInfo, OpCode::OP_NullCheck,
    OpCode::OP_Throw, OpCode::OP_MakeBoundFunc,
    OpCode::OP_MakeVFunc,
    OpCode::OP_SlotInitDone, OpCode::OP_SlotInitFail};
static constexpr OpCode kStrideKindWord[] = {
    //v0.13.1: OP_TypeNameArg is u16 dst + u8 ordinal — word-then-tag
    //(OP_Neg is tag-then-word); the group encodes only the 4-byte extent.
    OpCode::OP_Neg, OpCode::OP_BitNot, OpCode::OP_TypeNameArg};
static constexpr OpCode kStrideKindByte[] = {
    OpCode::OP_PrimCast};
static constexpr OpCode kStrideTwoWords[] = {
    OpCode::OP_JumpIfNot, OpCode::OP_Concat_str, OpCode::OP_Eq_str,
    OpCode::OP_Ne_str, OpCode::OP_Less_str, OpCode::OP_LessEqual_str,
    OpCode::OP_Greater_str, OpCode::OP_GreaterEqual_str,
    OpCode::OP_StrLen, OpCode::OP_CallMethod,
    OpCode::OP_CallMethodDirect, OpCode::OP_CallIntrinsic,
    OpCode::OP_CallDelegate, OpCode::OP_Eq_func, OpCode::OP_Ne_func,
    OpCode::OP_Eq_struct, OpCode::OP_Ne_struct,
    OpCode::OP_ArrayLength, OpCode::OP_ArrayShape,
    OpCode::OP_LoadStatic, OpCode::OP_StoreStatic,
    OpCode::OP_ClassInitDone, OpCode::OP_ClassInitFail,
    //v0.13.1: OP_CheckCast gained its second u16 (descIdx) — classIdx,
    //descIdx, the same extent family as the two-word loads.
    OpCode::OP_SlotInitGate, OpCode::OP_TypeNameCompose, OpCode::OP_CheckCast,
    //v2.23: OP_MakeFunc gained its second u16 (descIdx) — funcIdx,
    //descIdx; the descriptor row pairs the instantiation with the
    //handle (kInstDescNone sentinel when the target is non-generic).
    OpCode::OP_MakeFunc,
    //v0.13.9 phase ③: the ref-value family — dst/src word pairs (RefOfLocal
    //and RefOfPromoted build the pair, RefHandle extracts the base handle).
    OpCode::OP_RefOfLocal, OpCode::OP_RefOfPromoted, OpCode::OP_RefHandle};
static constexpr OpCode kStrideKindTwoWords[] = {
    OpCode::OP_Add, OpCode::OP_Sub, OpCode::OP_Mul, OpCode::OP_Div,
    OpCode::OP_Mod, OpCode::OP_Shl, OpCode::OP_Shr,
    OpCode::OP_BitAnd, OpCode::OP_BitOr, OpCode::OP_BitXor,
    //v0.13.9 phase ③: the ref dereference pair — kind byte + dst/src words.
    OpCode::OP_LoadRef, OpCode::OP_StoreRef,
    //Task 8: the T& read and store — ordinal byte + dst/src words
    //(same extent).
    OpCode::OP_LoadRef_T, OpCode::OP_StoreRef_T};
static constexpr OpCode kStrideKindThreeWords[] = {
    //Task 13: the generic-body field bind — ordinal byte + dst/base/cell
    //words, the three-word twin of the kind-two-words family.
    OpCode::OP_RefOfField_T};
static constexpr OpCode kStrideThreeWords[] = {
    //v0.13.1: OP_New gained its third u16 (descIdx) — dst, classIdx,
    //descIdx, the same extent family as AllocStruct's three words.
    //0.13.1a: OP_CallStatic gained the same third u16 — funcIdx,
    //callParamBase, descIdx (the receiver-less direct call fills a
    //generic callee's frame descriptor cell from the call site).
    OpCode::OP_New, OpCode::OP_CallStatic,
    OpCode::OP_AllocStruct, OpCode::OP_LoadField,
    OpCode::OP_StoreField, OpCode::OP_CopyStruct,
    OpCode::OP_LoadElementFlat, OpCode::OP_StrByteAt,
    OpCode::OP_ClassInitGate, OpCode::OP_CheckArrayRank,
    OpCode::OP_CopyStructInPlace,
    //v0.13.9 phase ③: the field/element ref builders — three word operands.
    OpCode::OP_RefOfField, OpCode::OP_RefOfElement};
static constexpr OpCode kStrideFourWords[] = {
    OpCode::OP_LoadElementN, OpCode::OP_StoreElementN,
    OpCode::OP_LoadElementDyn, OpCode::OP_StoreElementDyn,
    OpCode::OP_RefOfElementN,
    //v0.13.1: OP_CheckArrayType gained its descIdx word — dst, src,
    //arrayTypeIdx, descIdx; OP_AllocArrayDyn gained its element
    //descriptor word the same way (Array(dims) allocations stamp too).
    //D6: the T equality pair — dst, ordinal, lhs, rhs (0xFFFF = the
    //null-literal side).
    OpCode::OP_CheckArrayType, OpCode::OP_AllocArrayDyn,
    OpCode::OP_Eq_T, OpCode::OP_Ne_T,
    OpCode::OP_StrForeachStep};
static constexpr OpCode kStrideFiveWords[] = {
    //v0.13.1: the array allocators gained their descIdx word —
    //AllocArrayN: dst, typeIdx, rank, dimsBase, descIdx; AssembleArrayN:
    //dst, typeIdx, elemsBase, count, descIdx.
    OpCode::OP_MakeArrayView,
    OpCode::OP_AllocArrayN, OpCode::OP_AssembleArrayN};
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

static constexpr StrideExtentGroup kStrideGroups[] = {
    StrideGroup(kStrideNoOperands, 1),
    StrideGroup(kStrideByteTag, 1 + 1),
    StrideGroup(kStrideOneWord, 1 + 2),
    StrideGroup(kStrideKindWord, 1 + 1 + 2),
    StrideGroup(kStrideKindByte, 1 + 1 + 1),
    StrideGroup(kStrideTwoWords, 1 + 2 + 2),
    StrideGroup(kStrideKindTwoWords, 1 + 1 + 2 + 2),
    StrideGroup(kStrideKindThreeWords, 1 + 1 + 2 + 2 + 2),
    StrideGroup(kStrideThreeWords, 1 + 2 + 2 + 2),
    StrideGroup(kStrideFourWords, 1 + 2 + 2 + 2 + 2),
    StrideGroup(kStrideFiveWords, 1 + 2 + 2 + 2 + 2 + 2),
    StrideGroup(kStrideCmpHead, 1 + 1 + 1 + 2 + 2),
    StrideGroup(kStrideConst32, 1 + 4),
    StrideGroup(kStrideConst64, 1 + 8)};

//Compile-time completeness gate: every OpCode must sit in exactly one
//extent group. A missing row (a new opcode never registered — the name
//table's static_assert cannot see this table) or a duplicate (one
//opcode in two groups) is an authoring bug, caught here at build time
//instead of the runtime throw below.
constexpr bool StrideGroupsCoverAllOpCodes() {
    for (size_t op = 0; op < static_cast<size_t>(OpCode::OP_Count); ++op) {
        size_t hits = 0;
        for (const StrideExtentGroup& group : kStrideGroups)
            for (size_t i = 0; i < group.count; ++i)
                if (group.ops[i] == static_cast<OpCode>(op))
                    ++hits;
        if (hits != 1)
            return false;
    }
    return true;
}
static_assert(StrideGroupsCoverAllOpCodes(),
    "every OpCode must appear in exactly one stride extent group");

inline size_t InstructionStride(OpCode op) {
    for (const StrideExtentGroup& group : kStrideGroups)
        for (size_t i = 0; i < group.count; ++i)
            if (group.ops[i] == op)
                return group.stride;
    throw std::runtime_error("unknown opcode in disassembler");
}

} // namespace nlang
