# Bytecode Instructions

This page is the reference table for the NLang instruction set: the
compiler (`VmBackend`) translates the abstract syntax tree (AST) into these opcodes, and the
executor (`VmExecutor`) interprets them one at a time. Most opcodes read
and write the result register `pResult` and local variable slots. The
`OP_*` names are real source identifiers you can use as search anchors.

Numeric opcodes do not encode the type in their name: a single opcode
carries the operand type as a `u8 kind` immediate (the scalar RTK code,
dispatched through registry-instantiated operation tables). See
[Binary Expression Promotion](binary-expression-promotion.md) for how
the compiler picks the kind.

### Core

| Opcode          | Operands                    | Description                    |
|-----------------|-----------------------------|--------------------------------|
| OP_ConstInt32   | int32                       | Push constant to pResult       |
| OP_ConstFloat   | float                       | Push constant to pResult       |
| OP_ConstInt64   | int64                       | Push 8-byte constant to pResult |
| OP_ConstDouble  | double                      | Push 8-byte constant to pResult |
| OP_ConstZero    | —                           | Push 0 to pResult              |
| OP_ConstString  | uint16 constIdx             | Push string constant handle    |
| OP_VarLocal     | uint16 offset               | Load local to pResult          |
| OP_Assign       | uint16 dst                  | Store pResult to local         |
| OP_Return       | —                           | Return from function           |
| OP_Stop         | —                           | Halt execution                 |
| OP_AssertFail   | uint16 msgIdx               | Throw "assertion failed: <msg>" |

### Arithmetic

| Opcode      | Operands              | Description              |
|-------------|-----------------------|--------------------------|
| OP_Add      | kind, dst, src        | locals[dst] += locals[src] |
| OP_Sub      | kind, dst, src        | locals[dst] -= locals[src] |
| OP_Mul      | kind, dst, src        | locals[dst] *= locals[src] |
| OP_Div      | kind, dst, src        | locals[dst] /= locals[src] |
| OP_Mod      | kind, dst, src        | locals[dst] %= locals[src]; float rows compute fmod |
| OP_Neg      | kind, dst             | locals[dst] = -locals[dst] |
| OP_Shl      | kind, dst, src        | locals[dst] <<= count; the count must lie in [0, bit width) or the VM raises a named error |
| OP_Shr      | kind, dst, src        | Same, arithmetic on signed rows and logical on unsigned rows |

`kind` is the scalar RTK immediate selecting the row of the
registry-instantiated operation table (int32, float, byte, ulong, ...).

### Comparison & Logical

| Opcode          | Operands              | Description                        |
|-----------------|-----------------------|------------------------------------|
| OP_Cmp          | kind, cmpOp, lhs, rhs | int32 0/1 written to locals[lhs]; cmpOp ∈ {0 Less, 1 LessEqual, 2 Greater, 3 GreaterEqual, 4 Equal, 5 NotEqual} |
| OP_LogicalNot   | dst                   | locals[dst] = !locals[dst]         |

`&&` and `||` have no dedicated instruction — they are lowered at codegen
to a short-circuit jump sequence (`OP_JumpIfNot` plus double
`OP_LogicalNot` normalization); `OP_LogicalNot` is what implements `!`.
String and function-value comparisons ride their own opcodes (see
String and Function Values below).

### Control Flow

| Opcode       | Operands              | Description                    |
|--------------|-----------------------|--------------------------------|
| OP_Jump      | uint16 target         | Unconditional jump             |
| OP_JumpIfNot | uint16 target, uint16 localOff | Jump if the int32 local == 0 |

### Struct & Class

| Opcode            | Operands                        | Description              |
|-------------------|---------------------------------|--------------------------|
| OP_AllocStruct    | dst, structIdx, fieldCount      | Allocate struct on heap  |
| OP_CopyStruct     | dst, src, structIdx             | Deep-copy struct         |
| OP_New            | dst, classIdx                   | Allocate class on heap   |
| OP_LoadField      | dst, obj, fieldOff              | Read field from heap     |
| OP_StoreField     | obj, fieldOff, src              | Write field to heap      |
| OP_NullCheck      | obj                             | Throw if null            |

### Array

| Opcode          | Operands                        | Description              |
|-----------------|---------------------------------|--------------------------|
| OP_AllocArray   | dst, arrayTypeIdx, sizeSlot     | Allocate array on heap   |
| OP_LoadElement  | dst, arr, index                 | Read element             |
| OP_StoreElement | arr, index, src                 | Write element            |
| OP_ArrayLength  | dst, arr                        | Element count → int32    |

### Function Calls

| Opcode                 | Operands                    | Description              |
|------------------------|-----------------------------|--------------------------|
| OP_CallFunc            | funcIdx, callParamBase      | Call function            |
| OP_CallMethodDirect    | funcIdx, callParamBase      | Call non-virtual method  |
| OP_CallMethod          | methodNameIdx, callParamBase| Virtual dispatch by name |
| OP_CallIntrinsic       | intrinsicId, callParamBase  | Invoke intrinsic by ID   |
| OP_CallFuncOut         | funcIdx, callParamBase, outMask (uint32) | + out writeback |
| OP_CallMethodDirectOut | funcIdx, callParamBase, outMask (uint32) | + out writeback |
| OP_ParaEnd             | —                           | End of parameter list    |

### Function Values

| Opcode              | Operands                             | Description                     |
|---------------------|--------------------------------------|---------------------------------|
| OP_MakeFunc         | funcIdx (uint16)                     | Static handle {funcIdx, 0, 0}   |
| OP_MakeBoundFunc    | funcIdx (uint16)                     | Reads receiver from pResult → {funcIdx, this, static} |
| OP_MakeVFunc        | nameIdx (uint16, string constant table) | Reads receiver from pResult → {nameIdx, this, virtual} |
| OP_CallDelegate     | calleeLocal, callParamBase           | Invoke through a handle         |
| OP_CallDelegateOut  | calleeLocal, callParamBase, outMask (uint32) | + out writeback         |
| OP_Eq_func          | lhs, rhs                             | Handle content equality (null-guarded) |
| OP_Ne_func          | lhs, rhs                             | Handle content inequality       |
| OP_Func_to_str      | pResult accumulator                  | "func <name>" / "method <name>"; null → "<null>" |

See [First-Class Function Values](first-class-function-values.md#first-class-function-values)
for the handle layout and dispatch semantics.

### String

| Opcode      | Operands    | Description              |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | Concatenate strings      |
| OP_Eq_str   | lhs, rhs    | String equality          |
| OP_Ne_str   | lhs, rhs    | String inequality        |
| OP_Less_str | lhs, rhs    | Bytewise relational (Unicode Transformation Format (UTF-8) byte order == code point order); a null operand compares as the empty constant |
| OP_LessEqual_str | lhs, rhs | Bytewise `<=`            |
| OP_Greater_str | lhs, rhs | Bytewise `>`             |
| OP_GreaterEqual_str | lhs, rhs | Bytewise `>=`       |
| OP_StrLen   | dst, src    | String length (byte count) |
| OP_StrByteAt | dst, str, idx | Read the idx-th byte of the string as ubyte; out of range raises IndexOutOfBoundsException |
| OP_StrForeachStep | str, off, cond, ch | One code-point iteration step: decode the code point at byte offset `off` into `ch`, advance `off` by its encoded length, write the 1/0 continue flag to `cond` |

### Boxing / Unbox / Downcast

| Opcode       | Operands             | Description                                  |
|--------------|------------------------|----------------------------------------------|
| OP_Box       | typeKind (uint8)       | Box primitive (in pResult) as Object ref; always allocates an RTK_Boxed heap slot. Null literals are never boxed — the compiler skips the wrap so null stays the 0 sentinel. |
| OP_Unbox     | typeKind (uint8)       | Unwrap boxed primitive from pResult (heap idx). Verify RTK_Boxed tag matches; throw on null/invalid, non-boxed target or tag mismatch. |
| OP_CheckCast | classIdx (uint16)     | Verify pResult (heap idx) is classIdx or subclass (walk runtime super chain). Throw on mismatch. Push ref back unchanged. |

OP_Box/OP_Unbox use the `pResult` register convention — read input from
pResult, write output back to pResult. The implicit-cast emit path
(FixupExprType) wraps primitives in SnCastExpr with TCK_Box; the explicit
`expr as T` operator creates SnAsExpr whose codegen emits
OP_Unbox/OP_CheckCast depending on the resolved cast kind.

### Type Cast

| Opcode           | Operands              | Description              |
|------------------|-----------------------|--------------------------|
| OP_PrimCast      | srcKind, dstKind      | Convert pResult in place between any two scalar rows the resolver admits (int↔float, widenings, constant-fit narrowings) |
| OP_Prim_to_str   | kind                  | Format the scalar in pResult via its row's ValueToString, mint a string object, write the handle back (bool renders true/false, char encodes its code point) |
| OP_Enum_to_str   | enumDefIdx (uint16)   | int32 enum value → string (name lookup) |
| OP_Array_to_str  | —                     | Format the array in pResult as "[e1, e2, ...]" |

The string-coercion opcodes follow the same pResult convention as the
numeric casts: read source from `pResult`, mint the formatted string as
a string object, write the new handle (int32) back to `pResult`; the
emit path then stores it with `OP_Assign dst`.

`OP_Enum_to_str` takes a uint16 `enumDefIdx` immediate operand. It reads the
int32 enum value from `pResult`, looks up `m_compiledModule.enumNames[enumDefIdx][value]`,
mints the name as a string object, and writes the handle back. Throws
if the value is out of range.

Emit sites:
- `EmitExprCast.cpp` `Access(SnCastExpr&)` emits `OP_PrimCast` /
  `OP_Prim_to_str` for every implicit scalar conversion the resolver
  wrapped — including the binary `+` coercion when the other operand is
  string (enum routes to `OP_Enum_to_str` before the int collapse).
- `EmitExprMemberString.cpp` dispatches `.toString()` on the receiver's
  type: enum → `OP_Enum_to_str`, any scalar → `OP_Prim_to_str`, array →
  `OP_Array_to_str`, string → identity no-op.

### Exceptions

| Opcode        | Operands   | Description                                  |
|---------------|------------|----------------------------------------------|
| OP_Throw      | uint16 src | Throw the Exception object at locals[src]    |
| OP_Rethrow    | —          | Rethrow the current catch's exception        |
| OP_PopHandler | —          | Pop one handler entry at catch exit          |

### Switch

| Opcode     | Operands       | Description                    |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | Marker (no runtime effect)    |
| OP_Case    | uint16 nextOff  | Marker (patched by compiler)  |

A switch compiles to a chain of per-label comparisons and conditional
jumps — there is no jump-table opcode. The comparison opcode is chosen
from the discriminant's **family** (not its static type):

- int family (int + enums, which compare as their int values) →
  `OP_Cmp` Equal
- float family → `OP_Cmp` Equal on the float row (IEEE `==`: `-0.0 == 0.0` is true, NaN
  never matches)
- string family → `OP_Eq_str` (byte-content compare, constant-table
  order is irrelevant)

Multi-value clauses (`case 1, 2:`) emit one comparison per label: every
label's test jumps to the clause body on hit and to the next label's
test on miss; a single-label clause degenerates to a two-jump
shape. Because a clause carries 2+N jumps of four distinct target
kinds (clause exit / next label / body / implicit exit), the jump
back-patcher (the clause-exit fixup) walks a variable-length record list
rather than locating records at a fixed stride. Each case body
ends with an implicit jump out of the switch (Java/C# no-fall-through);
an explicit `break` additionally pops handlers, as it may leave catch
regions lexically.

### Enum Method Calling Convention

Enum methods reuse the class-method call path with one convention:
**`this` is the enum's int32 value, not a heap reference**.

- Calls emit `OP_CallMethodDirect funcIdx callParamBase` after the
  receiver expression is evaluated into the evaluation scratch area's
  slot 0 (receiver-first shape, same as `s.equals`).
- The callee frame's `this` local is allocated with typeKind
  `RTK_Int32` (class methods use `RTK_Class`). This matters: the garbage collection (GC)
  root scan walks locals by typeKind — an enum `this` typed as a class
  would be traced as a heap index and corrupt the heap.
- Representation decision: enums stay nominal-int32 at runtime —
  `==`, `switch`, argument passing, and `.ncu` serialization are all
  untouched. Java-style heap-singleton enums would need a module-level
  instance-init subsystem (init function execution order + GC roots)
  and are not supported.

### Misc

| Opcode       | Operands       | Description              |
|--------------|----------------|--------------------------|
| OP_DebugInfo | uint16 line    | Debug line info (and debugger checkpoint) |
