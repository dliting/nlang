#pragma once
#include <cstdint>

namespace nlang {

enum class OpCode : uint8_t {
    // === Control flow ===
    OP_Return,          // return from function
    OP_Jump,            // int16 target_offset
    OP_JumpIfNot,       // int16 target_offset, uint16 local_offset
    OP_Stop,            // halt execution

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

    // === Debug ===
    OP_DebugInfo,       // uint16 info

    OP_Count
};

const char* OpCodeName(OpCode op);

} // namespace nlang
