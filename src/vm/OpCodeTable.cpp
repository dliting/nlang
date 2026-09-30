#include "BytecodeOps.h"
#include <iterator>  //std::size for the positional-table static_assert

namespace nlang {

static const char* s_OpCodeNames[] = {
    "return",
    "jump",
    "jump_if_not",
    "stop",
    "assert_fail",
    "const_i32",
    "const_f32",
    "const_str",
    "const_zero",
    "const_i64",
    "const_f64",
    "var_local",
    "assign",
    "prim_cast",
    "prim_to_str",
    "enum_to_str",
    "array_to_str",
    "add",
    "sub",
    "mul",
    "div",
    "mod",
    "neg",
    "cmp",
    "not",
    "call",
    "para_end",
    "switch",
    "case",
    "concat_str",
    "eq_str",
    "ne_str",
    "lt_str",
    "le_str",
    "gt_str",
    "ge_str",
    "strlen",
    "debug",
    "alloc_struct",
    "load_field",
    "store_field",
    "copy_struct",
    "new",
    "call_method",
    "call_method_direct",
    "call_intrinsic",
    "null_check",
    "alloc_array",
    "load_element",
    "store_element",
    "array_length",
    "box",
    "unbox",
    "check_cast",
    "throw",
    "rethrow",
    "pop_handler",
    "call_out",
    "call_method_direct_out",
    "make_func",
    "call_delegate",
    "eq_func",
    "ne_func",
    "func_to_str",
    "make_bound_func",
    "make_vfunc",
    "call_delegate_out",
    "str_byte_at",
    "str_foreach_step",
};

const char* OpCodeName(OpCode op) {
    auto idx = static_cast<size_t>(op);
    if (idx < static_cast<size_t>(OpCode::OP_Count))
        return s_OpCodeNames[idx];
    return "unknown";
}

//The name table is positional: a missing or extra row is not a compile
//error but an off-by-N mislabel (and a potential OOB read before this
//bound existed). Bind the size to the enum so the two cannot drift.
static_assert(std::size(s_OpCodeNames) == static_cast<size_t>(OpCode::OP_Count),
    "s_OpCodeNames is positional and must have exactly one row per OpCode");

} // namespace nlang
