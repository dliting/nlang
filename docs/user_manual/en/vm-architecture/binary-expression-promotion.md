# Binary Expression Promotion

The VM's arithmetic opcodes carry the operand type as a `u8 kind`
immediate on a single opcode (`OP_Add`, `OP_Sub`, `OP_Cmp`, `OP_Shl`,
...), while the language lets you mix the numeric types freely. Binary
expression promotion is the mechanism that bridges the gap: at compile
time both operands are unified to one common type, then the opcode
emits that type as its kind immediate. This page describes how the
rules are implemented in the resolver and in codegen.

## Promotion rules

Arithmetic binary expressions (`+ - * / %`) use **symmetric numeric
promotion**: both operands are promoted to a common type before the
op. The common type comes from the scalar-type registry
(`CommonNumericType`): integer rows narrower than `int` promote to
`int` first (the C# rule — `byte + byte` is `int`); among the
remaining integer rows the result is the smallest type that can
implicitly receive both operands (`int + uint` is `long`, `uint +
ulong` is `ulong`); any float operand beats any integer and the wider
float rank wins (`float + double` is `double`). A pair with no
implicit common type is a compile error
(`no implicit common type for "Int32" and "ULong".`). `string` joins
`+` with any operand (the non-string side is wrapped and formatted at
run time), other operators on `string` are compile errors, and `null`
is not a valid arithmetic operand
(`null is not a valid arithmetic operand.`).

Shift (`<<` `>>`) is the asymmetric exception: the left operand's row
decides the result type (a narrower-than-`int` left operand promotes
to `int`), and the count must convert to that type implicitly — a
wider or differently-signed count is rejected
(`operator '<<' requires a count implicitly convertible to "Int32", got "UInt32".`),
while a constant that fits the type is accepted. The VM range-checks
the count at run time.

Comparison (`< <= > >= == !=`) and logical (`&& ||`) expressions
resolve to `bool` (carried as int32 0/1). Comparison first runs the
type gates — `string` compares only with `string` or `null`, function
values and arrays only with `==`/`!=`, `bool` and `char` have their
own misuse gates — then promotes mixed numeric operands to their
common type the same way arithmetic does (`1 < 2.5` compares as
`double`); same-type pairs, class/enum identity comparisons and
`null` comparisons skip the wrap.

## Resolver

(`ExprResolverBinary.cpp` `Access(SnBinaryExpr&)`):
1. Dispatch: comparison and logical operators run the gates and
   resolve to NK_Bool; shift goes to `ResolveShiftBinary`; everything
   else to `ResolveArithmeticBinary`.
2. Compute T_result per the rules above.
3. Set `sn.EvalDataType(T_result)`.
4. Wrap each operand via `FixupExprType` in a `SnCastExpr` if its type
   differs from T_result. After wrap, `sn.Children()[0]/[1]` hold the
   (possibly cast) operands; `sn.Left()/Right()` are stale but unused.

## Codegen

(`EmitExprBinary.cpp` binary path): iterates `sn.Children()` rather
than `Left()/Right()`, and dispatches the opcode on
`leftChild.EvalDataType()` (enum normalizes to int32). Because the
resolver wrapped both operands, the left child's kind equals T_result
for arithmetic and shift, and the common type for mixed comparison.
`string` selects the dedicated opcodes (`OP_Concat_str`, `OP_Eq_str`,
the `OP_Less_str` family — bytewise Unicode code point order),
function handles
select `OP_Eq_func`/`OP_Ne_func`, and everything else goes through the
kind-immediate family (`OP_Add` and friends; `OP_Cmp` additionally
carries the compare operator as an immediate). `%` rides the same
table: integer rows compute `%`, float rows compute fmod. `&&`/`||`
lower to short-circuit branches (`OP_JumpIfNot` plus double
`OP_LogicalNot` normalization) and claim no eval-area slot; the binary
path parks the right operand in a per-level `EvalAreaClaim`.

## FixupExprType and EvalDataType

A `SnCastExpr` created by `FixupExprType` always gets `EvalDataType`
set, and so does the explicit `expr as T` path. Wrapper nodes for
implicit casts (assignment / param / binary promotion) therefore all
carry a definite type, and downstream consumers never see a null type.
A wrap that loses precision reports a precision-loss warning,
suppressible with `--no-warn`.

No new opcodes. Reuses the existing kind-immediate family (`OP_Add`,
`OP_Cmp`, `OP_Shl`, ...) and the single cast opcode `OP_PrimCast`
(source and destination kinds as immediates).
