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

### Built-in Generic `List<T>` (Phase 8e-3)

`List<T>` is a built-in generic class implementing **erasure semantics**
(Java model): the type parameter `T` exists only at compile time. At
runtime, all `List<X>` instantiations share a single backing
CompiledClass ("List") with one hidden field `__handle` of type int.

**Compile-time model.** The resolver maintains a per-(baseName, typeArgs)
cache of synthetic SnClassDecl instances:

```
static std::map<std::tuple<std::string, std::vector<SnField*>>,
                SnClassDecl*> s_genericInstances;
```

Each cache miss mints a new SnClassDecl named e.g. `"List<int>"` whose
methods carry **substituted signatures** (T → int / Point / ...) for
static type checking. A polymorphic-signature registry drives
substitution:

| Method     | Param slots    | Return slot |
|------------|----------------|-------------|
| Add        | [T]            | void        |
| Get        | [int]          | T           |
| Set        | [int, T]       | void        |
| Length     | []             | int         |
| RemoveAt   | [int]          | void        |
| IndexOf    | [T]            | int         |
| Contains   | [T]            | int         |
| Clear      | []             | void        |

A `m_bIsGenericInst = true` flag on the SnClassDecl marks synthetic
instances so codegen knows to emit `OP_Box` for primitive-T method
parameters and `OP_Unbox` for primitive-T Get() returns.

**Runtime storage.** List elements live in a side table:

```
struct ListSlot {
    std::vector<int32_t> elements;   // heap idxs (boxed primitives or class refs)
};

std::vector<ListSlot>   m_listStore;       // index = __handle - 1 (0 reserved for null)
std::vector<int32_t>    m_listFreeList;    // recycled slots after GC sweep
```

All elements are heap indices uniformly — primitive T values are boxed
at the call site (`OP_Box typeKind` before `OP_CallMethod`), class-T
values pass through unchanged. This trades storage density for
uniformity: GC tracing has no per-element kind check.

**Intrinsics (9 new IDs).** List methods are dispatched through
`OP_CallMethod` like any class method, but the function backing each
name is a no-op stub that triggers `ExecuteIntrinsic`:

| Intrinsic ID            | Behavior                                                |
|-------------------------|---------------------------------------------------------|
| `INTR_List_Ctor`        | Allocate ListSlot, store idx in `this.__handle`         |
| `INTR_List_Add`         | Read boxed value heap idx from param, push_back         |
| `INTR_List_Get`         | Read int idx, return elements[idx]                      |
| `INTR_List_Set`         | Read int idx + heap idx, replace elements[idx]          |
| `INTR_List_Length`      | Return elements.size()                                  |
| `INTR_List_RemoveAt`    | Read int idx, elements.erase(...)                       |
| `INTR_List_IndexOf`     | Linear search; return position or -1                    |
| `INTR_List_Contains`    | Linear search; return 1 / 0                             |
| `INTR_List_Clear`       | elements.clear()                                        |

**GC integration.** MarkPhase walks class-instance children normally;
when it encounters an instance whose slot[0] (`classIdx`) equals the
cached `m_listClassIdx`, it additionally reads `__handle` (slot[1]) and
marks every entry in `m_listStore[__handle-1].elements` as a heap
reference. Out-of-bounds and free indices are skipped.

SweepPhase mirrors this: when a List instance is collected, its
`__handle` is pushed onto `m_listFreeList` for reuse by the next
`INTR_List_Ctor`. The ListSlot itself is not freed (it may have live
references from other List instances after handle reuse is impossible —
in practice, since handle is recycled only when *this* List dies, the
slot's contents are unreachable).

**Codegen: NewExpr with constructor aliasing fix.** When `new T(args)`
appears as a method-call argument, the naive emission (evaluate
constructor args into callParamBase, then OP_New into resultOffset)
clobbers the same callParamBase slot. The fix: after evaluating
constructor args, allocate the OP_New result into the slot *after* the
last param (`callParamBase + paramIdx * VALUE_SIZE`), then copy to
resultOffset:

```
// args evaluated into callParamBase[1..N]
allocSlot = callParamBase + N * VALUE_SIZE
OP_New allocSlot, classIdx
OP_CallMethodDirect ctorIdx, callParamBase   // ctor reads this=allocSlot
OP_VarLocal allocSlot
OP_Assign resultOffset                        // copy to caller's expected slot
```

### Built-in Generic `Dict<K,V>` (Phase 8e-4)

`Dict<K,V>` mirrors `List<T>`'s architecture: **erasure semantics**,
single backing CompiledClass ("Dict") across all instantiations, one
hidden field `__handle` of type int. The compilation cache key becomes
`(baseName, typeArgs)` with `typeArgs.size()` == 2; the runtime class
is shared regardless of K and V.

**Runtime storage.** Entries live in a side table:

```
struct DictSlot {
    std::vector<std::pair<int32_t,int32_t>> entries;  // (K heap idx, V heap idx)
};

std::vector<DictSlot>   m_dictStore;       // index = __handle - 1
std::vector<int32_t>    m_dictFreeList;    // recycled slots after GC sweep
int16_t                 m_dictClassIdx;    // cached at module load
```

Both K and V are heap indices uniformly — primitive K/V values are
boxed at the call site (`OP_Box typeKind` before `OP_CallMethod`).
Lookup is **linear scan** O(n); the open-addressing hashtable
optimization is a future phase.

**Intrinsics (7 new IDs 53-59).**

| Intrinsic ID           | Behavior                                                  |
|------------------------|-----------------------------------------------------------|
| `INTR_Dict_Ctor`       | Allocate DictSlot, store idx in `this.__handle`           |
| `INTR_Dict_Set`        | Linear scan; if key exists replace V, else append (K,V)   |
| `INTR_Dict_Get`        | Linear scan; throw "Dict key not found" if absent         |
| `INTR_Dict_ContainsKey`| Linear scan; return 1 / 0                                 |
| `INTR_Dict_Remove`     | Linear scan; erase if found, return 1 / 0                 |
| `INTR_Dict_Clear`      | entries.clear()                                           |
| `INTR_Dict_Count`      | Return entries.size()                                     |

**Kind-aware equality.** `DictKeysEqual(k1, k2)` is the central
helper used by Set/Get/ContainsKey/Remove:

1. Identity fast path (`k1 == k2`).
2. Bounds and kind-match check (`m_slotKinds[k1] == m_slotKinds[k2]`).
3. Branch on kind:
   - `RTK_Class` / `RTK_Struct`: identity (compare heap idxs).
   - `RTK_Boxed`: branch on inner type tag (`m_structHeap[k][0]`):
     - `RTK_Int32` / `RTK_Float`: compare value bits at
       `m_structHeap[k][kBoxedValueSlot]` (IEEE 754 — `NaN != NaN`).
     - `RTK_String`: compare `m_stringPool[bits]` content (value eq).

This pattern generalizes the Phase 8e-3 fix-up C2 fix (List IndexOf/
Contains value-bit comparison) to multi-tag keys.

**Codegen: per-method boxing plan.** Replaces the List-specific
codegen block. When the call target's class is a generic instantiation
(`SnClassDecl::IsGenericInstantiation()`), the codegen builds a per-
method plan based on `(baseName, methodName, typeArgs)`:

```
struct ArgBoxPlan { uint8_t tag; bool needsBox; };
std::map<uint16_t, ArgBoxPlan> argPlans;   // paramIdx -> plan
bool    returnsBoxed = false;
uint8_t returnTag    = 0;
```

- List dispatch: `Add` → slot 1; `Set` → slot 2; `IndexOf`/`Contains`
  → slot 1; `Get` → returnsBoxed.
- Dict dispatch: `Set` → K at slot 1 + V at slot 2; `Get` → K at slot
  1 + returnsBoxed (V); `ContainsKey`/`Remove` → K at slot 1.

A shared helper `BoxingTagFor(SnField*)` returns a
`BoxingTagResult {tag, isPrimitive}` so that `RTK_Int32 == 0` no
longer collides with "no boxing" — the `isPrimitive` bool is the
authoritative signal. The param loop emits `OP_Box <tag>` before
`OP_CallMethod` for any slot in `argPlans`; after the call, `OP_Unbox
<tag>` is emitted if `returnsBoxed`.

**GC integration.** MarkPhase: when class-instance slot[0] equals
`m_dictClassIdx`, read `__handle` (slot[1]) and mark both K and V of
every entry as heap references. Out-of-bounds / free indices are
skipped. SweepPhase: when a Dict instance is collected, push its
`__handle` onto `m_dictFreeList` for reuse by the next `INTR_Dict_Ctor`.

**ReadHandle uniform validation.** `ReadDictHandle(callParamBase,
locals, methodName)` mirrors `ReadListHandle`: reads `thisHeapIdx`
from the param base, throws "Dict `<method>` on null instance" if
`thisHeapIdx <= 0`, throws "...on stale reference" if it exceeds the
heap size, and throws "...on uninitialized instance" if the handle
slot is 0. All 6 post-ctor intrinsics route through this helper.

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

## Foreach Statement Lowering (Phase 8e-5)

`foreach (Type var in iterable) { body }` compiles to **index-based
expansion** — no new opcode is introduced. The 12-step lowering mirrors
the `for` loop pattern (line 1991 of VmBackend.cpp) with a body-prelude
that loads element `i` into the user variable slot.

### Hidden locals (uniquified for nesting)

Each `foreach` allocates four hidden locals before the `LoopContext`
push, named with a per-function counter to avoid `AllocLocal`'s dedup-
by-name collision in nested loops:

| Local | typeKind | Purpose |
|-------|----------|---------|
| `<varName>` | derived from element type | user-visible loop variable |
| `__foreach_iter_<N>` | `RTK_Array` (Array) or `RTK_Class` (List/Dict) | iterable reference |
| `__foreach_i_<N>` | `RTK_Int32` | loop counter |
| `__foreach_n_<N>` | `RTK_Int32` | cached length |

`<N>` comes from `FuncContext::foreachCounter`, which is reset to 0 at
function entry.

### Codegen 3-way branch

The kind of iterable is detected at codegen time (not resolver time),
preserving the user-visible AST:

- **Array** (`T[N]`): iterable is `SnIdentifierExpr` whose `Field` has
  `IsArrayType()`. Length via `OP_ArrayLength`; element via
  `OP_LoadElement` (with `OP_CopyStruct` for struct-element types).
- **List<T>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="List"` and `IsGenericInstantiation()`. Length via
  `OP_CallMethod "Length"`; element via `OP_CallMethod "Get"` followed
  by `OP_Unbox` for primitive T (per-method boxing plan).
- **Dict<K,V>**: iterable's `EvalDataType()` is `SnClassDecl` with
  `BaseName()=="Dict"`. **Inline `Keys()` call** materializes a fresh
  `List<K>` into `iterSlot` first (step 2b), then the rest mirrors the
  List path with element type K.

The `typeKind` of each hidden local is what GC uses at safepoints to
identify reference roots, so `iterSlot` must be `RTK_Array` for the
Array path and `RTK_Class` for List/Dict — incorrect tags would cause
either leaked references (root missed) or spurious tracing of integer
slots as heap idxs.

### `Dict.keys()` intrinsic

`INTR_Dict_Keys = 60` (CompiledModule.h). Registered as a method on the
Dict class with `paramCount=1, returnTypeKind=RTK_Class`. The VmExecutor
handler:

1. Reads the dict handle via `ReadDictHandle` (uniform null/stale/uninit
   validation).
2. `AllocClassOnHeap(m_listClassIdx)` — fresh List class instance.
3. `AllocListHandle()` — fresh List side-table slot.
4. Wires the handle into `m_structHeap[heapIdx][kListHandleFieldOffset]`.
5. Copies `dict.entries[i].first` (the K heap idxs) into
   `m_listStore[handle-1].elements`.
6. Writes `listHeapIdx` to `pResult`; sets `m_gcPending = true`.

The resulting `List<K>` is traced automatically by the existing GC
`MarkPhase` (it already handles `classIdx == m_listClassIdx`). The K
heap idxs already exist in the dict and are tracked through it; the new
List holds additional references to the same objects, which is safe
(GC mark bits dedupe).

### LoopContext reuse

`m_loopStack` already supports `break`/`continue` for `for`/`while`/`do`
and `switch`. `foreach` pushes a `LoopContext{isSwitch=false}` and uses
the same break/continue patch logic. `continue` jumps to the post-body
"i = i + 1" site; `break` jumps to the loop end.

## Init List Lowering (Phase 8e-6)

Collection initializers (`[1,2,3]`, `new Type{...}`) lower through a
single `NK_InitListExpr` handler in `VmBackend::EmitExpression`. The
handler dispatches on the resolved `EvalDataType` plus the
`TargetIsArray` flag set by the resolver.

### Resolver data flow

`StatementResolver::Access(SnAssignStmt&)` peeks at the RHS before
resolving it. If the RHS is a bare `SnInitListExpr` (no `ExplicitType`)
without an `InferredTarget`, the LHS variable is recorded on the init
list via `InferredTarget(pLeftField)`. The resolver then visits the
RHS; `ExprResolveAccessor::Access(SnInitListExpr&)` reads either the
explicit `ExplicitType()` or the `InferredTarget()`, sets
`EvalDataType` to the resolved target field, and copies array-ness
into `TargetIsArray` (needed because `EvalDataType` on an array
variable returns the element type — the array-ness would otherwise be
lost).

The resolver does not propagate expected types to child init lists
(recursive type inference for `[[1,2],[3]]` is not yet supported).
Children must use the explicit `new Type{...}` form to carry their
own type.

### Codegen dispatch table

| Target kind                 | Allocation             | Per-entry store                      |
|-----------------------------|------------------------|--------------------------------------|
| `T[]` array                 | `OP_AllocArray` size=N | `OP_ConstInt32 i; OP_StoreElement`  |
| `List<T>`                   | `OP_New "List"` + ctor | `OP_Box` (if T primitive); `OP_CallMethod "Add"` |
| `Dict<K,V>`                 | `OP_New "Dict"` + ctor | `OP_ConstString key`; `OP_Box` (if V primitive); `OP_CallMethod "Set"` |
| user class                  | `OP_New` + no-arg ctor | `OP_StoreField <offset>`            |
| struct                      | `OP_AllocStruct`       | `OP_StoreField <offset>`            |

### Per-method boxing plan reuse

The `List<T>` and `Dict<K,V>` branches reuse the per-method boxing
infrastructure from Phase 8e-3/8e-4: `BoxingTagFor(typeArg)` returns
`{tag, isPrimitive}`, and `OP_Box` is emitted before `Add`/`Set` only
when the element type is primitive. This keeps initialization consistent
with `lst.add(x)` and `d.set(k, v)` calls written by hand.

### Temp slot allocation

The handler writes the collection to `resultOffset` (provided by the
caller, e.g. `tempSlot2` for struct-assignment codegen). Each entry's
value is evaluated to a distinct slot chosen by `PickTempSlot(resultOffset)`
so that nested init lists (which recursively enter this handler) do not
clobber the parent's value slot.

### Reuse of existing opcodes

No new opcodes were added for Phase 8e-6. Allotment, element stores,
method calls, and field stores all reuse Phase 3/4/8e-3/8e-4 primitives.
The init-list handler is pure orchestration: allocate once, then
populate via the existing stores.

## Binary Expression Promotion (Phase 8e-8)

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

## Known Limitations

1. **Exit code range**: Process exit codes are 8-bit (0-255) on Windows.
   Test values must not exceed 255.
2. **No short-circuit evaluation**: `&&` and `||` evaluate both operands.
3. **No super() call**: Ancestor constructors are not automatically invoked.
4. **No struct methods**: Structs are data-only. Use classes for behavior.
5. **No user-defined generics**: `class Foo<T> { ... }` is not supported.
   Only built-in generic classes (`List<T>`, `Dict<K,V>`) are
   recognized by the compiler.
6. **`List<int>` storage overhead**: each primitive element is boxed into
   a heap slot (`RTK_Boxed`). For value-heavy lists, an `IntList`
   specialization with unboxed storage is the planned escape hatch
   (deferred until profiling shows real need).
7. **String pool grows unbounded**: Concatenated strings are added to the pool
   but never collected.
