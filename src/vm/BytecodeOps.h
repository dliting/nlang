#pragma once
#include <cstdint>

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

    // === Variable access (byte-offset into locals) ===
    OP_VarLocal,        // uint16 offset, read locals[offset..] to pResult
    OP_Assign,          // uint16 dst_offset, write pResult to locals[dst_offset..]

    // === Type conversion ===
    OP_CastIntToFloat,  // read int32 from pResult, write float to pResult
    OP_CastFloatToInt,  // read float from pResult, write int32 to pResult
    OP_Int32_to_str,    // Phase 8e-9a: read int32 from pResult, format decimal,
                        // push to m_stringPool, write new string idx (int32) to pResult
    OP_Float_to_str,    // Phase 8e-9a: read float from pResult, format with "%g",
                        // push to m_stringPool, write new string idx (int32) to pResult
    OP_Enum_to_str,     // Phase 8e-9b: uint16 enumDefIdx immediate; read int32 enum value
                        // from pResult, lookup m_compiledModule.enumNames[enumDefIdx][value],
                        // push name to m_stringPool, write new string idx (int32) to pResult
    OP_Array_to_str,    // Phase 9b-pre: no operands; read array heap idx from pResult,
                        // format as "[e1, e2, ...]", push to m_stringPool, write idx to pResult

    // === Arithmetic - int32 (dst += src) ===
    OP_Add_i32,         // uint16 dst, uint16 src, locals[dst] += locals[src]
    OP_Sub_i32,         // uint16 dst, uint16 src
    OP_Mul_i32,         // uint16 dst, uint16 src
    OP_Div_i32,         // uint16 dst, uint16 src
    OP_Mod_i32,         // uint16 dst, uint16 src
    OP_Neg_i32,         // uint16 dst, locals[dst] = -locals[dst]

    // === Arithmetic - float ===
    OP_Add_f32,         // uint16 dst, uint16 src
    OP_Sub_f32,         // uint16 dst, uint16 src
    OP_Mul_f32,         // uint16 dst, uint16 src
    OP_Div_f32,         // uint16 dst, uint16 src
    OP_Neg_f32,         // uint16 dst

    // === Comparison - int32 (result: int32 0 or 1 written to locals[lhs]) ===
    OP_Less_i32,        // uint16 lhs, uint16 rhs
    OP_LessEqual_i32,
    OP_Greater_i32,
    OP_GreaterEqual_i32,
    OP_Equal_i32,
    OP_NotEqual_i32,

    // === Comparison - float ===
    OP_Less_f32,
    OP_LessEqual_f32,
    OP_Greater_f32,
    OP_GreaterEqual_f32,
    OP_Equal_f32,
    OP_NotEqual_f32,

    // === Logical ===
    OP_LogicalAnd,      // uint16 lhs, uint16 rhs
    OP_LogicalOr,       // uint16 lhs, uint16 rhs
    OP_LogicalNot,      // uint16 dst, locals[dst] = !locals[dst]

    // === Function call ===
    OP_CallFunc,        // uint16 func_index, uint16 call_param_base; call function, result to pResult
    OP_ParaEnd,         // mark end of parameter evaluation

    // === Switch/case ===
    //Reference: EN's I_Base_Switch/I_Base_Case (Compiler.h).
    OP_Switch,          // uint16 switch_local_offset (the switch value slot)
    OP_Case,            // uint16 jump_to_next (placeholder, patched by FixChainedJumps)
                        // After OP_Case: condition comparison code + OP_JumpIfNot to next case,
                        // then case body code.

    // === String operations ===
    OP_Concat_str,      // uint16 dst, uint16 src — string concatenation
    OP_Eq_str,          // uint16 lhs, uint16 rhs — string equality
    OP_Ne_str,          // uint16 lhs, uint16 rhs — string inequality
    OP_StrLen,          // uint16 dst, uint16 src — string length → int32

    // === Debug ===
    OP_DebugInfo,       // uint16 info

    // === Struct operations ===
    OP_AllocStruct,     // uint16 dst, uint16 structIdx, uint16 fieldCount
    OP_LoadField,       // uint16 dst, uint16 obj, uint16 fieldOff
    OP_StoreField,      // uint16 obj, uint16 fieldOff, uint16 src
    OP_CopyStruct,      // uint16 dst, uint16 src, uint16 structIdx

    // === Class operations ===
    OP_New,             // uint16 dst, uint16 classIdx — allocate object on heap
    OP_CallMethod,      // uint16 methodNameStringIdx, uint16 callParamBase — virtual method dispatch (name-based)
    OP_CallMethodDirect,// uint16 funcIdx, uint16 callParamBase — non-virtual method call
    OP_CallIntrinsic,   // uint16 intrinsicId, uint16 callParamBase — intrinsic function call
    OP_NullCheck,       // uint16 obj — throw if locals[obj] is null/invalid (hard crash, like Java NPE)

    // === Array operations ===
    OP_AllocArray,      // uint16 dst, uint16 arrayTypeIdx, uint16 sizeSlot
    OP_LoadElement,     // uint16 dst, uint16 arr, uint16 index
    OP_StoreElement,    // uint16 arr, uint16 index, uint16 src
    OP_ArrayLength,     // uint16 dst, uint16 arr

    // === Phase 8e-1: Object protocol + boxing/cast ===
    OP_Box,             // uint8 typeKind — pop primitive, push boxed Object ref
    OP_Unbox,           // uint8 typeKind — pop Object ref, type-check, push primitive
    OP_CheckCast,       // uint16 classIdx — pop ref, verify subclass, throw on mismatch

    // === Phase 9d: Exception handling ===
    OP_Throw,           // uint16 src — throw Exception at locals[src] (heap idx)
    OP_Rethrow,         // no operands — rethrow current catch's exception
    OP_PopHandler,      // no operands — pop one entry from handlerExcStack at catch exit

    OP_Count
};

const char* OpCodeName(OpCode op);

} // namespace nlang
