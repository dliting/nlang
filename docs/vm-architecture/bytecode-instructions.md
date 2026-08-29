# Bytecode Instructions


### Core

| Opcode          | Operands                    | Description                    |
|-----------------|-----------------------------|--------------------------------|
| OP_ConstInt32   | int32                       | Push constant to pResult       |
| OP_ConstFloat   | float                       | Push constant to pResult       |
| OP_ConstZero    | —                           | Push 0 to pResult              |
| OP_ConstString  | uint16 poolIdx              | Push string pool index         |
| OP_VarLocal     | uint16 offset               | Load local to pResult          |
| OP_Assign       | uint16 dst                  | Store pResult to local         |
| OP_Return       | —                           | Return from function           |

### Arithmetic

| Opcode      | Operands          | Description              |
|-------------|-------------------|--------------------------|
| OP_Add_i32  | dst, src          | locals[dst] += locals[src] |
| OP_Sub_i32  | dst, src          | locals[dst] -= locals[src] |
| OP_Mul_i32  | dst, src          | locals[dst] *= locals[src] |
| OP_Div_i32  | dst, src          | locals[dst] /= locals[src] |
| OP_Mod_i32  | dst, src          | locals[dst] %= locals[src] |
| OP_Neg_i32  | dst               | locals[dst] = -locals[dst] |
| OP_Add_f32  | dst, src          | Same for float            |
| OP_Sub_f32  | dst, src          |                          |
| OP_Mul_f32  | dst, src          |                          |
| OP_Div_f32  | dst, src          |                          |
| OP_Neg_f32  | dst               |                          |

### Comparison & Logical

| Opcode          | Operands    | Description                        |
|-----------------|-------------|------------------------------------|
| OP_Less_i32     | lhs, rhs    | locals[lhs] = (a < b) ? 1 : 0     |
| OP_Equal_i32    | lhs, rhs    | locals[lhs] = (a == b) ? 1 : 0    |
| ...             |             | (same pattern for all comparisons) |
| OP_LogicalAnd   | lhs, rhs    | locals[lhs] = (a && b) ? 1 : 0    |
| OP_LogicalOr    | lhs, rhs    | locals[lhs] = (a || b) ? 1 : 0    |
| OP_LogicalNot   | dst         | locals[dst] = !locals[dst]         |

### Control Flow

| Opcode       | Operands              | Description                    |
|--------------|-----------------------|--------------------------------|
| OP_Jump      | int16 target          | Unconditional jump             |
| OP_JumpIfNot | int16 target, uint16  | Jump if local == 0             |

### Struct & Class

| Opcode            | Operands                        | Description              |
|-------------------|---------------------------------|--------------------------|
| OP_AllocStruct    | dst, structIdx, fieldCount      | Allocate struct on heap  |
| OP_CopyStruct     | dst, src, structIdx             | Deep-copy struct         |
| OP_New            | dst, classIdx                   | Allocate class on heap   |
| OP_LoadField      | dst, obj, fieldOff              | Read field from heap     |
| OP_StoreField     | obj, fieldOff, src              | Write field to heap      |
| OP_NullCheck      | obj                             | Throw if null            |

### Function Calls

| Opcode              | Operands                    | Description              |
|---------------------|-----------------------------|--------------------------|
| OP_CallFunc         | funcIdx, callParamBase      | Call function            |
| OP_CallMethodDirect | funcIdx, callParamBase      | Call non-virtual method  |
| OP_CallMethod       | methodNameIdx, callParamBase| Virtual dispatch by name |
| OP_CallIntrinsic    | intrinsicId, callParamBase  | Invoke intrinsic by ID   |

### Function Values (Phase 13)

| Opcode              | Operands                             | Description                     |
|---------------------|--------------------------------------|---------------------------------|
| OP_MakeFunc         | funcIdx (uint16)                     | Static handle {funcIdx, 0, 0}   |
| OP_MakeBoundFunc    | funcIdx (uint16)                     | Reads receiver from pResult → {funcIdx, this, static} |
| OP_MakeVFunc        | nameIdx (uint16, string pool)        | Reads receiver from pResult → {nameIdx, this, virtual} |
| OP_CallDelegate     | calleeLocal, callParamBase           | Invoke through a handle         |
| OP_CallDelegateOut  | calleeLocal, callParamBase, outMask (uint32) | + out writeback         |
| OP_Eq_func          | lhs, rhs                             | Handle content equality (null-guarded) |
| OP_Ne_func          | lhs, rhs                             | Handle content inequality       |
| OP_Func_to_str      | accumulator form                     | "func N" / "method N"; null → "<null>" |

See [First-Class Function Values](first-class-function-values.md#first-class-function-values-phase-13)
for the handle layout and dispatch semantics.

### String

| Opcode      | Operands    | Description              |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | Concatenate strings      |
| OP_Eq_str   | lhs, rhs    | String equality          |
| OP_Ne_str   | lhs, rhs    | String inequality        |
| OP_Less_str | lhs, rhs    | Bytewise relational (Phase 11 Step 3b; UTF-8 byte order == code point order) |
| OP_LessEqual_str | lhs, rhs | Bytewise `<=`            |
| OP_Greater_str | lhs, rhs | Bytewise `>`             |
| OP_GreaterEqual_str | lhs, rhs | Bytewise `>=`       |
| OP_StrLen   | dst, src    | String length (byte count, Phase 11 decision #7) |

### Boxing / Unbox / Downcast (Phase 8e-1 + 8e-1.5)

| Opcode       | Operands             | Description                                  |
|--------------|------------------------|----------------------------------------------|
| OP_Box       | typeKind (uint8)       | Box primitive (in pResult) as Object ref; allocates RTK_Boxed heap slot. Value 0 short-circuits (null preservation). |
| OP_Unbox     | typeKind (uint8)       | Unwrap boxed primitive from pResult (heap idx). Verify RTK_Boxed tag matches; throw on mismatch. |
| OP_CheckCast | classIdx (uint16)     | Verify pResult (heap idx) is classIdx or subclass (walk runtime super chain). Throw on mismatch. Push ref back unchanged. |

OP_Box/OP_Unbox use the `pResult` register convention — read input from
pResult, write output back to pResult. The implicit-cast emit path
(FixupExprType) wraps primitives in SnCastExpr with TCK_Box; the explicit
`expr as T` operator (Phase 8e-1.5) creates SnAsExpr whose codegen emits
OP_Unbox/OP_CheckCast depending on the resolved cast kind.

### Type Cast

| Opcode           | Description              |
|------------------|--------------------------|
| OP_CastIntToFloat | int32 → float           |
| OP_CastFloatToInt | float → int32           |
| OP_Int32_to_str  | int32 → string (Phase 8e-9a, decimal via `std::to_string`) |
| OP_Float_to_str  | float → string (Phase 8e-9a, `%g` format) |
| OP_Enum_to_str   | int32 enum value → string (Phase 8e-9b, name lookup) |

The string coercion opcodes follow the same pResult convention as the int/
float casts: read source from `pResult`, push the formatted string to
`m_stringPool`, write the new string index (int32) back to `pResult`. The
emit pattern is always `OP_<type>_to_str` followed by `OP_Assign dst`.

`OP_Enum_to_str` takes a uint16 `enumDefIdx` immediate operand. It reads the
int32 enum value from `pResult`, looks up `m_compiledModule.enumNames[enumDefIdx][value]`,
pushes the name string to `m_stringPool`, and writes the index back. Throws
if the value is out of range.

Emit sites:
- `VmBackend.cpp` `EmitExpression(SnCastExpr&)` dispatches on
  `(srcKind, dstKind)` for int/float→string (implicit coercion).
- `VmBackend.cpp` `EmitExpression(SnMemberExpr&)` dispatches on
  `outer->EvalDataType()` for `.toString()` calls (enum→OP_Enum_to_str,
  int→OP_Int32_to_str, float→OP_Float_to_str, string→identity no-op).
- `VmBackend.cpp` `EmitExpression(SnCastExpr&)` for binary `+` coercion
  (enum/int/float→string when the other operand is string).

### Switch (Phase 12)

| Opcode     | Operands       | Description                    |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | Marker (no runtime effect)    |
| OP_Case    | uint16 nextOff  | Marker (patched by compiler)  |

A switch compiles to a chain of per-label comparisons and conditional
jumps — there is no jump-table opcode. The comparison opcode is chosen
from the discriminant's **family** (not its static type):

- int family (int + enums, which compare as their int values) →
  `OP_Equal` (i32)
- float family → `OP_Equal_f32` (IEEE `==`: `-0.0 == 0.0` is true, NaN
  never matches)
- string family → `OP_Eq_str` (byte-content compare, pool order is
  irrelevant)

Multi-value clauses (`case 1, 2:`) emit one comparison per label: every
label's test jumps to the clause body on hit and to the next label's
test on miss; a single-label clause degenerates to today's two-jump
shape. Because a clause carries 2+N jumps of four distinct target
kinds (clause exit / next label / body / implicit exit), the jump
back-patcher (`FixChainedJumps`) walks a variable-length record list —
the historical `i * 2` fixed-stride assumption is gone. Each case body
ends with an implicit jump out of the switch (Java/C# no-fall-through);
an explicit `break` additionally pops handlers, as it may leave catch
regions lexically.

### Enum Method Calling Convention (Phase 12)

Enum methods reuse the class-method call path with one convention:
**`this` is the enum's int32 value, not a heap reference**.

- Calls emit `OP_CallMethodDirect funcIdx callParamBase` after the
  receiver expression is evaluated into the claim area's slot 0
  (receiver-first shape, same as `s.equals`).
- The callee frame's `this` local is allocated with typeKind
  `RTK_Int32` (class methods use `RTK_Class`). This matters: the GC
  root scan walks locals by typeKind — an enum `this` typed as a class
  would be traced as a heap index and corrupt the heap.
- Representation decision (D3): enums stay nominal-int32 at runtime —
  `==`, `switch`, argument passing, and `.nmod` serialization are all
  untouched. Java-style heap-singleton enums would need a module-level
  instance-init subsystem (init function execution order + GC roots)
  and are deliberately deferred.

### Misc

| Opcode       | Operands       | Description              |
|--------------|----------------|--------------------------|
| OP_ParaEnd   | —              | End of parameter list    |
| OP_DebugInfo | uint16         | Debug line info          |
