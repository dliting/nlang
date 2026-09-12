# Garbage Collection Design


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

```text
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

### Array Field and Element Tracing (array redesign B)

- **Array-typed fields are declared, not inferred**: array struct/class
  fields store `RTK_Array` as their `fieldTypeKinds` entry in the
  `.nmod` (semantic floor v1.10). MarkPhase routes class and struct
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
  the field arms). This arm is unreachable from compilable source today
  (jagged declarations are rejected; `List<int[]>` elements live in the
  container store, traced by the List branch) — it is the correctness
  base for future or externally produced `.nmod` paths.

### Free List Integration

Allocation functions (AllocClassOnHeap, AllocStructOnHeap, DeepCopyStruct)
check the free list first. If a free slot is available, it's reused
(resize to correct size, set m_slotKinds/m_slotStructIdx). Otherwise,
emplace_back a new slot.

### GC Trigger Flow

```text
OP_New / OP_AllocStruct → set m_gcPending = true
                            ↓
Function entry (ExecuteFunction) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && heap > threshold → CollectGarbage()
```
