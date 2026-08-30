# Heap Architecture


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

Object has three virtual methods (`Equals(Object)→int`, `GetHashCode()→int`,
`toString()→string`), all dispatched via intrinsics:

| Intrinsic ID           | Behavior                                       |
|------------------------|------------------------------------------------|
| INTR_Object_Equals    | Identity: same heap idx → 1, else 0 (null==null→1) |
| INTR_Object_GetHashCode | Identity: heap idx of `this` (null→0)         |
| INTR_Object_toString  | `"ClassName@hex(heapIdx)"` (null→NPE)          |
| INTR_String_Equals    | Value: pool-content equality                   |
| INTR_String_GetHashCode | Value: `std::hash<std::string>` over content  |

The existing `OP_CallMethod` name-walk finds the most-derived implementation
first — there is no separate dispatch machinery for Object methods. When
the runtime reaches Object's intrinsic stub (no AST override exists), it
short-circuits to `ExecuteIntrinsic`.

**Two intrinsic dispatch paths (P3.2 fix)**: most intrinsics are reached
via `OP_CallMethod` / `OP_CallMethodDirect`, which inspect the
`callee.intrinsicId` field stamped on the `CompiledFunction` by VmBackend
and short-circuit to `ExecuteIntrinsic`. Strings, however, are primitives
(no class), so `string.getHashCode()` and `string.equals()` cannot go
through that path — VmBackend emits `OP_CallIntrinsic` directly for
them, which the executor dispatches via the same `ExecuteIntrinsic`
function. (Before commit 862d7a7, the `OP_CallIntrinsic` case threw
"intrinsic calls not yet implemented"; two e2e tests passed only because
the throw's exit code happened to equal the expected value.)

### Boxed Primitives (Phase 8e-1)

A primitive value (int/float/string) assigned to an Object-typed target is
boxed into a 2-slot heap entry:

```text
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

```text
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
