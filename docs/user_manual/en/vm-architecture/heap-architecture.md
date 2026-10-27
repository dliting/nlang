# Heap Architecture

Class, struct, boxed-primitive, function-value, and array records share
one heap array (`m_structHeap`); string objects live in a separate store
(`m_stringObjs`). Both are reclaimed by the garbage collector. This page
describes the heap's physical layout — slot structure, parallel arrays,
the string object store — and how the built-in generic containers
`List<T>`/`Dict<K,V>` are stored; it is the prerequisite for the
[Garbage Collection Design](garbage-collection-design.md) page.

### Single Heap Design

Struct and class objects share `m_structHeap: vector<StructSlot>` where
`StructSlot = vector<int32>`. This simplifies allocation and allows uniform
field access via `OP_LoadField`/`OP_StoreField`.

### Object Layout

**Struct**: `[field0, field1, ..., fieldN-1]`
- No type header (identified by `m_slotStructIdx` parallel array)
- Every data field occupies a uniform 2-cell (8-byte) slot so 64-bit
  scalars fit without per-field width logic; field i sits at cell 2i,
  i.e. field offset in bytecode = field_index * 8 bytes

**Class**: `[classIdx, field0, field1, ..., fieldN-1]`
- slot[0] = classIdx for runtime type identification (virtual dispatch)
- Field offset in bytecode = 4 + field_index * 8 bytes (the classIdx
  cell occupies the first 4 bytes)
- Inherited fields come before own fields (root ancestor first)

### Parallel Arrays

| Array           | Purpose                                    |
|-----------------|--------------------------------------------|
| m_slotKinds     | RTK_Class/RTK_Struct/RTK_Boxed/RTK_Func/RTK_Array/0=free per heap slot  |
| m_slotStructIdx | CompiledStruct index per struct slot       |
| m_markBits      | GC mark bit per heap slot                  |

`m_slotStructIdx` exists because struct objects have no type header. Without
it, MarkStruct cannot determine which CompiledStruct to consult for field
layout when tracing references. The alternative (adding a type header to
struct objects) would require shifting all LoadField/StoreField offsets by +1,
a larger and more error-prone change.

### Implicit `Object` Base Class

Every user class that does not explicitly inherit from another class has
`superClassIdx` set to the synthesized Object class's index by a post-pass
in `RegisterClasses`. Object is the only class with `superClassIdx == -1`.

Object has three virtual methods (`equals(Object)→int`, `getHashCode()→int`,
`toString()→string`), all dispatched via intrinsics:

| Intrinsic identifier (ID)           | Behavior                                       |
|------------------------|------------------------------------------------|
| INTR_Object_Equals    | Identity: same heap idx → 1, else 0 (null==null→1) |
| INTR_Object_GetHashCode | Identity: heap idx of `this` (null→0)         |
| INTR_Object_toString  | `"ClassName@hex(heapIdx)"` (null→null pointer exception (NPE))          |
| INTR_String_Equals    | Value: content equality                        |
| INTR_String_GetHashCode | Value: `std::hash<std::string>` over content  |

The existing `OP_CallMethod` name-walk finds the most-derived implementation
first — there is no separate dispatch machinery for Object methods. When
the runtime reaches Object's intrinsic stub (no abstract syntax tree (AST) override exists), it
short-circuits to `ExecuteIntrinsic`.

**Two intrinsic dispatch paths**: most intrinsics are reached
via `OP_CallMethod` / `OP_CallMethodDirect`, which inspect the
`callee.intrinsicId` field stamped on the `CompiledFunction` by VmBackend
and short-circuit to `ExecuteIntrinsic`. Strings, however, are primitives
(no class), so `string.getHashCode()` and `string.equals()` cannot go
through that path — VmBackend emits `OP_CallIntrinsic` directly for
them, which the executor dispatches via the same `ExecuteIntrinsic`
function.

### Boxed Primitives

A primitive value (int/float/string) assigned to an Object-typed target is
boxed into a 3-cell heap record:

```text
slot[0]     = type tag (RTK_Int32 / RTK_Float / RTK_String)
slots[1..2] = value bits (8-byte cell pair: int32 / float bits /
              string object handle; 4-byte kinds value-extend in the low cell)
```

`m_slotKinds[idx] = RTK_Boxed` (6). GC MarkPhase dispatches on the boxed
record's type tag: a string-tagged payload (slot[1]) is a handle into the
string object store and marks that string object; other tags carry raw
value bits with no outgoing references to trace. slot[0] is always read
as the type tag, never misinterpreted as a classIdx. SweepPhase frees
them like any other unreachable slot.

### Array Records

An array record's header carries the shape; the element area stores
data at the element stride (1 or 2 cells):

```text
slot[0] = RTK_Array (record kind)
slot[1] = arrayTypeIdx (array type table entry)
slot[2] = rank (dimension count; 1 for 1-D)
slot[3] = elemCount (total element count, product of the dimensions)
slots[4 .. 4+rank-1] = element count of each dimension
(header padded to an even cell count; 8-byte element families stay
naturally aligned)
element i = header cells + i * stride, laid out row-major
```

A 1-D array is the `rank=1` special case; rectangular
multi-dimensional arrays share the same layout — the element access
instructions fold multi-dimensional coordinates into a row-major
flat index, and `OP_ArrayShape` mints a shape array from the header
dimensions. GC marking resolves the element kind from
`arrayTypeIdx` and traces reference-shaped elements (class/struct/
string handles) as out-edges.

### The String Object Store

Strings are GC-managed immutable objects at run time, held in a separate
store `m_stringObjs`. The first five record kinds (class/struct/boxed/
func/array) share `m_structHeap`; strings are the sixth storage class,
with their own mark bit vector, free list, and collection threshold
(collection semantics on the [Garbage Collection Design](garbage-collection-design.md) page).

- **Handle semantics**: an `RTK_String` slot holds a 1-based handle
  (0 = null, reads as the empty string — the same sentinel convention
  as heap index 0).
- **Flat / Cons dual form**: an object is either Flat (`str` holds the
  content) or Cons (`left`/`right` hold two child handles).
  Concatenation (`OP_Concat_str`) allocates a Cons node, O(1) with zero
  copying; the first read flattens it in place into a Flat object —
  every existing handle then sees the flattened content.
- **immortal and interned bits**: `immortal` marks constant-materialized
  objects (never collected); `interned` marks objects registered in the
  short-string table (up to 40 bytes shared by content — a weak map
  from content to live handle). `==` between two interned strings
  reduces to a handle comparison.

### Built-in Generic `List<T>`

`List<T>` is a built-in generic class implementing **erasure semantics**
(Java model): the type parameter `T` exists only at compile time. At
runtime, all `List<X>` instantiations share a single backing
CompiledClass ("List") with one hidden field `__handle` of type int.

**Compile-time model.** The resolver maintains a cache of synthetic
SnClassDecl instances keyed by the full instantiation signature:

```cpp
struct GenericInstKey {
    std::string baseName;            // "List" / "Dict" / "func" / "Channel"
    std::vector<SnField*> typeArgs;  // resolved type-argument fields
    std::vector<uint8> outFlags;     // out-marked params (func)
};
// Array-ness needs no flag vector: an array-typed argument IS the
// interned SnArrayTypeToken in typeArgs, so pointer identity keeps
// List<int> and List<int[]> as distinct keys.
```

Each cache miss mints a new SnClassDecl named e.g. `"List<int>"`; its
member calls resolve to **substituted signatures** (T → int / Point /
...) for static type checking, driven by the type-argument side table.
A polymorphic-signature registry drives substitution:

| Method     | Param slots    | Return slot |
|------------|----------------|-------------|
| `add`      | [T]            | void        |
| `get`      | [int]          | T           |
| `set`      | [int, T]       | void        |
| `length`   | []             | int         |
| `removeAt` | [int]          | void        |
| `indexOf`  | [T]            | int         |
| `contains` | [T]            | int         |
| `clear`    | []             | void        |

A `m_bIsGenericInst = true` flag on the SnClassDecl marks synthetic
instances so codegen knows to emit `OP_Box` for primitive-T method
parameters and `OP_Unbox` for primitive-T `get()` returns — **except when
the type argument is an array type** (`List<int[]>`): an array-typed
argument is carried as the interned array-type token in the
instantiation's type-arg slot, whose runtime kind is `RTK_Array` (not a
primitive), so the boxing regions naturally treat its elements as raw
array handles with no boxing.

**Runtime storage.** List elements live in a side table:

```cpp
struct ListSlot {
    std::vector<int32_t> elements;   // heap idxs (boxed primitives, class refs, or raw array handles)
};

std::vector<ListSlot>   m_listStore;       // index = __handle - 1 (0 reserved for null)
std::vector<int32_t>    m_listFreeList;    // recycled slots after GC sweep
```

All elements are heap indices uniformly — primitive T values are boxed
at the call site (`OP_Box typeKind` before `OP_CallMethod`), class-T
and array-T values pass through unchanged. The GC does dispatch on each
element's runtime slot kind when tracing: reference-kind and boxed
elements (class/struct/boxed/array/func) are marked and pushed to the
worklist, so their children — class fields, boxed-string payloads, or,
for array elements, the array's own elements by elemKind — are traced
too.

**Intrinsics (9 IDs).** List methods are dispatched through
`OP_CallMethod` like any class method, but the function backing each
name is a no-op stub that triggers `ExecuteIntrinsic`:

| Intrinsic ID            | Behavior                                                |
|-------------------------|---------------------------------------------------------|
| `INTR_List_Ctor`        | Allocate ListSlot, store idx in `this.__handle`         |
| `INTR_List_Add`         | Read value heap idx from param (boxed primitive or raw handle), push_back |
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
reference, pushing reference-kind and boxed entries
(class/struct/boxed/array/func) onto the worklist so their children are
traced — an array element
pushed here reaches the worklist's `RTK_Array` arm, which traces the
array's own elements by elemKind (this is how `List<Point[]>` keeps
the `Point` records alive). Out-of-bounds and free indices are skipped.

SweepPhase mirrors this: when a List instance is collected, its
`__handle` is pushed onto `m_listFreeList` for reuse by the next
`INTR_List_Ctor`. The ListSlot itself is not freed (a handle is
recycled only after *this* List dies, so in practice no other List
instance can hold a live reference into the slot — its contents are
unreachable).

**Codegen: constructor invocation through the evaluation area.** A
constructor call (`new T(args)`, and the `T(args)` twin without `new`)
must not stage its arguments into `callParamBase` — allocating the new
object into `resultOffset` could clobber a staged argument. The
emission claims `{this, formals...}` in the evaluation area instead:
constructor bindings are evaluated into `claim[1..N]`, the object is
allocated straight into `resultOffset` (no conflict — the bindings live
in the evaluation area, not in `callParamBase`), the object is staged
as `claim[0]`, the whole claim is bulk-copied to `callParamBase`, and
`OP_CallMethodDirect` invokes the ctor:

```text
// ctor args evaluated into claim[1..N] (evaluation area)
OP_New resultOffset, classIdx              // OP_AllocStruct for a struct
claim[0] = resultOffset                    // the new object staged as `this`
bulk-copy claim -> callParamBase
OP_CallMethodDirect ctorIdx, callParamBase // ctor reads this=callParamBase[0]
```

### Built-in Generic `Dict<K,V>`

`Dict<K,V>` mirrors `List<T>`'s architecture: **erasure semantics**,
single backing CompiledClass ("Dict") across all instantiations, one
hidden field `__handle` of type int. The compilation cache key becomes
`(baseName, typeArgs)` with `typeArgs.size()` == 2; the runtime class
is shared regardless of K and V.

**Runtime storage.** Entries live in a side table:

```cpp
struct DictSlot {
    std::vector<std::pair<int32_t,int32_t>> entries;  // (K heap idx, V heap idx)
};

std::vector<DictSlot>   m_dictStore;       // index = __handle - 1
std::vector<int32_t>    m_dictFreeList;    // recycled slots after GC sweep
int16_t                 m_dictClassIdx;    // cached at module load
```

Both K and V are heap indices uniformly — primitive K/V values are
boxed at the call site (`OP_Box typeKind` before `OP_CallMethod`).
Lookup is a **linear scan**, O(n).

**Intrinsics (IDs 53-59, 7 total).**

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
     - scalar tags: compare value bits at
       `m_structHeap[k][kBoxedValueSlot]` — 8-byte rows (long/ulong)
       compare the high cell too (IEEE 754 — `NaN != NaN`).
     - `RTK_String`: compare the string-object content behind the
       handles (value eq).

This pattern generalizes the value-bit comparison of List IndexOf/
Contains to multi-tag keys.

**Codegen: per-method boxing plan.** Replaces the List-specific
codegen block. When the call target's class is a generic instantiation
(`SnClassDecl::IsGenericInstantiation()`), the codegen builds a per-
method plan based on `(baseName, methodName, typeArgs)`:

```cpp
struct ArgBoxPlan { uint8_t tag; bool needsBox; };
std::map<uint16_t, ArgBoxPlan> argPlans;   // paramIdx -> plan
bool    returnsBoxed = false;
uint8_t returnTag    = 0;
```

- List dispatch: `add` → slot 1; `set` → slot 2; `indexOf`/`contains`
  → slot 1; `get` → returnsBoxed.
- Dict dispatch: `set` → K at slot 1 + V at slot 2; `get` → K at slot
  1 + returnsBoxed (V); `containsKey`/`remove` → K at slot 1.
- Channel dispatch: `send` → T at slot 1; `recv` → returnsBoxed (T).

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
slot is 0. All 8 post-ctor Dict intrinsics (`set`/`get`/`containsKey`/
`remove`/`clear`/`count`/`keys`/`toString`) route through this helper.
