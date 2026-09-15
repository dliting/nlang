# Binary Expression Promotion (Phase 8e-8)


Arithmetic binary expressions (`+ - * / %`) use **symmetric numeric
promotion**: both operands are promoted to the wider type before the
op. Promotion rules: `int OP int → int`; `int OP float` / `float OP int
→ float`; `float OP float → float`; `string + string → string` (concat
only — other ops on string are compile errors).

**Resolver** (`ExprResolver.cpp` `Access(SnBinaryExpr&)` else branch):
1. Compute T_result per the rules above (string short-circuits to
   NK_String only for OP_Add; arithmetic prefers NK_Float if either
   operand is float; otherwise NK_Int32).
2. Set `sn.EvalDataType(T_result)`.
3. Wrap each operand via `FixupExprType` in a `SnCastExpr` if its type
   differs from T_result. After wrap, `sn.Children()[0]/[1]` hold the
   (possibly cast) operands; `sn.Left()/Right()` are stale but unused.

**Codegen** (`VmBackend.cpp` binary path): iterates `sn.Children()`
rather than `Left()/Right()`, and dispatches the opcode on
`leftChild.EvalDataType()`. For arithmetic, leftChild is the SnCastExpr
whose EvalDataType is T_result; for comparison (no wrap), leftChild is
the original operand whose type selects the i32/f32/str variant
(`OP_Eq_str` for `string == string` even though `bin.EvalDataType()`
is always NK_Int32 for comparisons).

**FixupExprType fix**: `FixupExprType` now sets `EvalDataType` on the
newly-created `SnCastExpr` (Phase 8e-8). Previously only the explicit
`expr as T` path set this; implicit casts created via FixupExprType
(assignment / param / binary promotion) left EvalDataType unset, which
made downstream consumers see null type.

No new opcodes. Reuses `OP_CastIntToFloat` / `OP_CastFloatToInt` /
`OP_Add_f32` etc. from earlier phases.
