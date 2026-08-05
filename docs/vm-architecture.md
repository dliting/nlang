# NLang VM Architecture

## Overview

NLang uses a register-based bytecode VM. The compiler (VmBackend) translates
the AST to bytecode, and the executor (VmExecutor) interprets it.

## Compilation Pipeline

```
AST → VmBackend → CompiledModule (.nmod)
                      ↓
              BytecodeEmitter → bytecode
              AllocLocal → LocalDescriptor[] + frame layout
```

### Compilation Phases (GenerateStatements)

1. **RegisterStructs** — register struct types, resolve fieldStructIndices
2. **RegisterClasses** — register class types, resolve superClassIdx,
   fieldClassIndices, fieldStructIndices
3. **ResolveStructClassRefs** — resolve struct fieldClassIndices (deferred
   because classes aren't registered during RegisterStructs)
4. **RegisterFunctions** — register function signatures
5. **PopulateClassMethods** — map class methods to function indices
6. **GenerateAllBytecode** — emit bytecode for all functions

### Why ResolveStructClassRefs is separate

Structs can contain class-typed fields, but `RegisterStructs` runs before
`RegisterClasses`. At that point, `FindClass` returns -1 because classes
aren't registered yet. The solution: store type names in `m_structFieldTypeNames`
during RegisterStructs, then resolve class indices in a separate pass after
RegisterClasses.

## Stack Frame Layout

Each function invocation has a local variable frame:

```
[this (methods)] [params...] [returnSlot] [tempSlot] [tempSlot2] [callParamBase(8 slots)] [user locals...]
```

All slots are 4 bytes (VALUE_SIZE). The frame is a flat byte array indexed
by offset. `LocalDescriptor` records each variable's offset, size, and
typeKind for GC root scanning.

### Temp Slots

- **tempSlot**: Used for binary op right operand, condition evaluation, and
  intermediate values in member access chains.
- **tempSlot2**: Second temp for nested binary expressions. When resultOffset
  == tempSlot, the right operand goes to tempSlot2 to avoid overwriting.

### Call Parameter Area

8 slots (32 bytes) at `callParamBase`. For method calls, slot 0 is `this`.
Arguments are evaluated left-to-right into this area before the call.

## Heap Architecture

### Single Heap Design

Struct and class objects share `m_structHeap: vector<StructSlot>` where
`StructSlot = vector<int32>`. This simplifies allocation and allows uniform
field access via `OP_LoadField`/`OP_StoreField`.

### Object Layout

**Struct**: `[field0, field1, ..., fieldN-1]`
- No type header (identified by `m_slotStructIdx` parallel array)
- Field offset in bytecode = field_index * 4

**Class**: `[classIdx, field0, field1, ..., fieldN-1]`
- slot[0] = classIdx for runtime type identification (virtual dispatch)
- Field offset in bytecode = (field_index + 1) * 4 (accounts for classIdx)
- Inherited fields come before own fields (root ancestor first)

### Parallel Arrays

| Array           | Purpose                                    |
|-----------------|--------------------------------------------|
| m_slotKinds     | RTK_Class/RTK_Struct/RTK_Boxed/0=free per heap slot  |
| m_slotStructIdx | CompiledStruct index per struct slot       |
| m_markBits      | GC mark bit per heap slot                  |

`m_slotStructIdx` exists because struct objects have no type header. Without
it, MarkStruct cannot determine which CompiledStruct to consult for field
layout when tracing references. The alternative (adding a type header to
struct objects) would require shifting all LoadField/StoreField offsets by +1,
a larger and more error-prone change.

### Implicit `Object` Base Class (Phase 8e-1)

Every user class that does not explicitly inherit from another class has
`superClassIdx` set to the synthesized Object class's index by a post-pass
in `RegisterClasses`. Object is the only class with `superClassIdx == -1`.

Object has two virtual methods (`Equals(Object)→int`, `GetHashCode()→int`),
both dispatched via intrinsics:

| Intrinsic ID           | Behavior                                       |
|------------------------|------------------------------------------------|
| INTR_Object_Equals    | Identity: same heap idx → 1, else 0 (null==null→1) |
| INTR_Object_GetHashCode | Identity: heap idx of `this` (null→0)         |
| INTR_String_Equals    | Value: pool-content equality                   |
| INTR_String_GetHashCode | Value: `std::hash<std::string>` over content  |

The existing `OP_CallMethod` name-walk finds the most-derived implementation
first — there is no separate dispatch machinery for Object methods. When
the runtime reaches Object's intrinsic stub (no AST override exists), it
short-circuits to `ExecuteIntrinsic`.

### Boxed Primitives (Phase 8e-1)

A primitive value (int/float/string) assigned to an Object-typed target is
boxed into a 2-slot heap entry:

```
slot[0] = type tag (RTK_Int32 / RTK_Float / RTK_String)
slot[1] = value bits (int32 / float bits / string pool idx)
```

`m_slotKinds[idx] = RTK_Boxed` (6). GC MarkPhase skips RTK_Boxed slots
entirely — they hold no outgoing references, so traversal would be wasted
work and would misinterpret the type tag in slot[0] as a classIdx. SweepPhase
frees them like any other unreachable slot.

## Bytecode Instructions

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

### String

| Opcode      | Operands    | Description              |
|-------------|-------------|--------------------------|
| OP_Concat_str | dst, src  | Concatenate strings      |
| OP_Eq_str   | lhs, rhs    | String equality          |
| OP_Ne_str   | lhs, rhs    | String inequality        |
| OP_StrLen   | dst, src    | String length            |

### Boxing (Phase 8e-1)

| Opcode   | Operands              | Description                          |
|----------|------------------------|--------------------------------------|
| OP_Box   | dst, typeKind, src     | Box primitive as Object (RTK_Boxed) |
| OP_Unbox | (reserved, 8e-1.5)     | Unbox Object to primitive            |
| OP_CheckCast | (reserved, 8e-1.5) | Runtime class downcast check         |

### Type Cast

| Opcode           | Description              |
|------------------|--------------------------|
| OP_CastIntToFloat | int32 → float           |
| OP_CastFloatToInt | float → int32           |

### Switch

| Opcode     | Operands       | Description                    |
|------------|----------------|--------------------------------|
| OP_Switch  | uint16 localOff | Marker (no runtime effect)    |
| OP_Case    | uint16 nextOff  | Marker (patched by compiler)  |

### Misc

| Opcode       | Operands       | Description              |
|--------------|----------------|--------------------------|
| OP_ParaEnd   | —              | End of parameter list    |
| OP_DebugInfo | uint16         | Debug line info          |

## Garbage Collection Design

### Design Decisions

Three key decisions with rationale documented in VmExecutor.h:

**1. Safepoint-triggered, not allocation-point-triggered**

At allocation points (OP_New/OP_AllocStruct), tempSlot and tempSlot2 may hold
active heap references (e.g., `a.b.c = new Foo()` where tempSlot=a, tempSlot2=a.b).
Tracking these temporaries would require per-instruction offset maps in
CompiledFunction, adding complexity.

Safepoints (function entry + loop back-edges) guarantee tempSlot/tempSlot2
are not holding active references. At function entry, the frame is freshly
set up. At loop back-edges, the condition is about to be re-evaluated,
consuming any temp values.

**2. Precise scan via LocalDescriptor, not conservative byte scan**

Conservative scanning (checking every 4-byte aligned value for valid heap
indices) couples GC to the physical stack frame layout (VALUE_SIZE, slot
alignment, tempSlot offsets). If the layout changes, GC breaks silently.

Precise scanning uses `LocalDescriptor.typeKind` to identify reference slots
and `LocalDescriptor.offset` to locate them. This is based on semantic
information, not physical layout. The GC remains correct regardless of frame
layout changes.

**3. m_slotStructIdx parallel array for struct type identification**

Struct objects have no type header (unlike class objects where slot[0] =
classIdx). MarkStruct needs the structIdx to look up field layout for
reference tracing. Options:
- Add a type header to struct objects: requires +1 offset shift in all
  LoadField/StoreField for structs, touching many instruction handlers
- Parallel array: only AllocStructOnHeap/DeepCopyStruct set it, no changes
  to existing instruction logic

The parallel array is the smaller, safer change.

**4. Iterative mark with worklist, not recursive**

Recursive MarkObject/MarkStruct can overflow the C++ call stack on deep object
chains (e.g., a linked list with 500+ nodes). The iterative approach uses a
`vector<int32_t>` worklist: MarkPhase identifies root references and pushes
them to the worklist, then processes entries iteratively until the worklist
is empty. Each entry's child references are pushed to the worklist if not
already marked. This bounds memory usage to O(reachable objects) with no
risk of stack overflow.

### GC Algorithm

```
CheckGCSafepoint():
  if m_gcPending && heap.size() > threshold:
    m_gcPending = false
    CollectGarbage()

CollectGarbage():
  MarkPhase()
  SweepPhase()

MarkPhase():
  clear all mark bits
  for each CallFrame:
    for each LocalDescriptor with typeKind in {RTK_Class, RTK_Struct}:
      read heap index from frame.locals + ld.offset
      if valid and not marked: set mark bit, push to worklist
    if pResult has reference return type:
      read heap index from pResult
      if valid and not marked: set mark bit, push to worklist
  while worklist not empty:
    pop entry from worklist
    if class: for each field, push unmarked reference children
    if struct: for each field, push unmarked reference children

SweepPhase():
  clear free list
  for each heap slot:
    if not free and not marked:
      if class: FreeOwnedStructs (free value-owned struct fields)
      if struct: FreeNestedStructs (free nested struct fields)
      clear slot, mark as free, add to free list
```

### Free List Integration

Allocation functions (AllocClassOnHeap, AllocStructOnHeap, DeepCopyStruct)
check the free list first. If a free slot is available, it's reused
(resize to correct size, set m_slotKinds/m_slotStructIdx). Otherwise,
emplace_back a new slot.

### GC Trigger Flow

```
OP_New / OP_AllocStruct → set m_gcPending = true
                            ↓
Function entry (ExecuteFunction) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && heap > threshold → CollectGarbage()
```

## Module Serialization

Compiled modules are saved as `.nmod` files with this layout:

```
"NLANGMOD"     magic (8 bytes)
uint16 majorVer = 1
uint16 minorVer = 0
string moduleName
string[] stringConstants
function[] functions
struct[] structs
class[] classes
```

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[].

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[], fieldAccess[],
methodIndices[], constructorIdx.

## Known Limitations

1. **Exit code range**: Process exit codes are 8-bit (0-255) on Windows.
   Test values must not exceed 255.
2. **No short-circuit evaluation**: `&&` and `||` evaluate both operands.
3. **No super() call**: Ancestor constructors are not automatically invoked.
4. **No struct methods**: Structs are data-only. Use classes for behavior.
5. **No array type**: Not yet implemented (Phase 4).
6. **No interface type**: Not yet implemented (Phase 6).
7. **String pool grows unbounded**: Concatenated strings are added to the pool
   but never collected.
