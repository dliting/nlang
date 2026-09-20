# Garbage Collection Design


### Design Decisions

Four key decisions with rationale documented in VmExecutor.h:

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

```text
CheckGCSafepoint():
  if m_gcPending && (heap.size() > threshold || strings.size() > strThreshold):
    m_gcPending = false
    CollectGarbage()

CollectGarbage():
  MarkPhase()
  SweepPhase()
  SweepStrings()

MarkPhase():
  clear all mark bits (heap and string stores)
  for each CallFrame:
    for each LocalDescriptor with typeKind in {RTK_Class, RTK_Struct, RTK_Array, RTK_Func}:
      read heap index from frame.locals + ld.offset
      if valid and not marked: set mark bit, push to worklist
    for each LocalDescriptor with typeKind == RTK_String:
      read string handle from frame.locals + ld.offset
      MarkString(handle)          //marks the object and its cons subtree
    if pResult has reference return type:
      read heap index from pResult
      if valid and not marked: set mark bit, push to worklist
    if pResult return type is RTK_String:
      MarkString(handle)          //conservative over-mark, kept as defense
  while worklist not empty:
    pop entry from worklist
    if class: for each field, push unmarked reference children;
      string-typed fields (RTK_String) mark their string handle instead
    if struct: for each field, push unmarked reference children;
      string-typed fields mark their string handle instead
    if array: push unmarked element records whose elemKind is a reference kind;
      string elements (elemKind == RTK_String) mark their string handle
    if boxed: a string-tagged payload (slot[1]) marks its string handle;
      other boxed tags carry raw bits (no children)

SweepPhase():
  clear free list
  for each heap slot:
    if not free and not marked:
      if class: FreeOwnedStructs (free value-owned struct fields)
      if struct: FreeNestedStructs (free nested struct fields)
      clear slot, mark as free, add to free list

SweepStrings():
  clear string free list
  for each string object (slot 0 is the null sentinel):
    if dead: continue
    if immortal (constant-materialized) or marked: count survivor, continue
    if interned: erase its entry from the short-string table
    reset slot to the dead-form sentinel, add to string free list
  strThreshold = max(strThreshold, 2 * survivorCount)
```

**Independent sweep for the string object store.** String objects live
in a separate store (see the heap architecture page) with its own mark
bit vector, free list, and collection threshold, but they reuse the
same `CollectGarbage`: one mark phase fills both bit vectors, and the
string sweep runs right after the heap sweep. Constant-materialized
objects carry the immortal bit and are never collected; a collected
interned string has its table entry purified in the same step, so the
at-most-one-live-interned-object-per-content invariant survives
collection.

**Threshold backoff for the string arm.** The string store's backing
vector never shrinks (dead slots recycle in place), so the size trigger
is level-triggered — past the first crossing every safepoint collects,
and each collection marks the entire live concatenation chain, degrading
total append cost to O(n^2) (measured: 5x10^4-node chain 6.6s, 10^5-node
chain 26.7s). Backing the threshold off to 2x the surviving population
after each sweep (`strThreshold = max(strThreshold, 2 * survivorCount)`)
makes
triggers advance geometrically with the live set: appends amortize to
O(1), with memory bounded at 2x the live set.

### Array Field and Element Tracing (array redesign B)

- **Array-typed fields are declared, not inferred**: array struct/class
  fields store `RTK_Array` as their `fieldTypeKinds` entry in the
  `.nmod` (semantic floor v1.11). MarkPhase routes class and struct
  field references with an explicit declared-kind + runtime-slot-kind
  double condition — `fieldTypeKinds[i] == RTK_Array` paired with the
  slot actually holding an array record — alongside the existing
  RTK_Class/RTK_Struct/RTK_Func arms.
- **The old runtime-kind fallback is gone**: MarkPhase no longer traces
  a field slot just because its runtime kind looks like an array. This
  is safe because jagged declarations (`T[][]`), the one source form
  that could smuggle an array record into a non-array-typed field slot,
  are rejected at resolve time by the compiler.
- **Defensive RTK_Array element arm**: the array branch traces elements
  whose declared `elemKind` is `RTK_Array` (same double condition as
  the field arms). Unreachable from compilable source today (jagged
  declarations are rejected) — it is the correctness base for future
  or externally produced `.nmod` paths. Array-typed *container*
  elements (`List<int[]>`, `Dict` keys/values) are traced by the
  container branch instead: the List/Dict arm marks and pushes
  reference-kind entries, so an array element reaches the worklist's
  `RTK_Array` arm, which traces the array's own elements by elemKind
  (this is how `List<Point[]>` keeps the `Point` records alive).

### Free List Integration

Allocation functions (AllocClassOnHeap, AllocStructOnHeap, DeepCopyStruct)
check the free list first. If a free slot is available, it's reused
(resize to correct size, set m_slotKinds/m_slotStructIdx). Otherwise,
emplace_back a new slot.

### GC Trigger Flow

```text
OP_New / OP_AllocStruct → set m_gcPending = true
String allocation (mint / concatenation node) → set m_gcPending = true
                            ↓
Function entry (ExecuteFunction) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && (heap > threshold || strings > strThreshold)
                   → CollectGarbage()
```
