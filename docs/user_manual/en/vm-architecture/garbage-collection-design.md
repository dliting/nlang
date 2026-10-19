# Garbage Collection Design

NLang's heap is managed automatically by garbage collection (GC):
developers create objects without freeing them by hand. This page
records the four core design decisions — safepoint triggering, precise
scanning with a conservative band, the parallel array, iterative
marking — plus the full mark-sweep algorithm, the root set for parked
contexts, and the tracing rules for array fields. Understanding
these decisions tells you when the VM collects and which slots count as
live.

### Design Decisions

Four key decisions with rationale documented in VmExecutor.h:

**1. Safepoint-triggered, not allocation-point-triggered**

At allocation points (OP_New/OP_AllocStruct), tempSlot and tempSlot2 may hold
active heap references (e.g., `a.b.c = Foo()` where tempSlot=a, tempSlot2=a.b).
Tracking these temporaries would require per-instruction offset maps in
CompiledFunction, adding complexity.

Safepoints (function entry + loop back-edges) put collection at
well-bounded positions: at function entry the frame is freshly set up, and
at loop back-edges the condition is about to be re-evaluated. Historically
this choice also leaned on the argument that back-edges never find active
references in the temp slots; with the conservative band scan (decision 2)
covering staging slots at every safepoint, that argument no longer carries
weight — the lasting value of safepoint triggering is a deterministic,
cost-bounded moment for the mark work.

**2. Precise scan for declared slots, conservative band for staging slots**

A conservative byte scan of the whole frame (testing every aligned value as
a candidate heap index) was the rejected option: it couples GC to the
physical stack frame layout (slot width, alignment, tempSlot offsets), and
if the layout changes, GC breaks silently. Declared slots therefore take
the precise route — `LocalDescriptor.typeKind` identifies reference slots,
`LocalDescriptor.offset` locates them — based on semantic information, not
physical layout, so correctness survives any frame layout change.

But the declaration table cannot see a frame's staging slots: nested-call
arguments and intermediate results in the evaluation scratch area have no
local descriptor. In `f(g(), h())`, g's return value lives only in the
caller's evaluation scratch area when h's frame is pushed (a safepoint),
invisible to the precise scan. The **conservative band scan** closes that
gap: over the frame's tail band past the parameter area, each 8-byte cell
gets a two-domain value test — a value inside the valid string-handle
range marks a string object, a value inside the valid heap-index range
enters the worklist. Parameter slots are excluded (their kinds are
declared); frames are zero-initialized, so an unwritten cell holds 0 and
hits neither domain. The collector never moves objects, so a cross-domain
false hit can only over-mark (an object lingers briefly), never under-mark.

A parked context's frames can be frozen mid-instruction, and their staging
values must survive exactly the same way — the band lets parked frames and
running frames share one three-arm scan (see "Parked contexts and the
scheduler root set"), with no separate root machinery for the parked shape.

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
  for each CallFrame (running stack):
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
    //conservative band: slots past the parameter area have no descriptor
    //(staged call arguments, evalArea claims, temp slots)
    for each 8-byte cell in [paramCount*8, localsSize):
      if value in string-handle range: MarkString(value)
      if value in heap-index range and not marked:
        set mark bit, push to worklist
  for each scheduler context:      //parked/queued contexts (steal model)
    for each parked CallFrame: run the same three arms (locals, result, band)
    if threadObjHeapIdx or failExcHeapIdx valid: mark, push to worklist
  for each host-registered root:
    string roots to MarkString, heap roots to worklist
  while worklist not empty:
    pop entry from worklist
    if class: for each field, push unmarked reference children;
      string-typed fields (RTK_String) mark their string handle instead;
      MarkChannelElements (buffered channel values are children)
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
in a separate store (see the [Heap Architecture](heap-architecture.md) page) with its own mark
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
total append cost to O(n^2) (benchmark: 5x10^4-node chain 6.6s,
10^5-node chain 26.7s). Backing the threshold off to 2x the surviving population
after each sweep (`strThreshold = max(strThreshold, 2 * survivorCount)`)
makes triggers advance geometrically with the live set: appends amortize to
O(1), with memory bounded at 2x the live set.

### Parked Contexts and the Scheduler Root Set

Under the frame-stack machine, the running context's frames ARE the call
stack; a **parked** context's whole frame stack (sleeping, waiting on a
join, contending a mutex, parked on an empty-channel receive, preempted)
lives in the scheduler, off the call stack. At collection time both are
roots:

- every parked context's every frame runs the same three scan arms as a
  running frame (declared locals, result cell, conservative band) — a park
  can freeze a frame mid-instruction, and its staging values and handler
  state must all survive;
- the context's `Thread` object (`join` identifies its target by handle)
  and a failed context's retained exception object (`join` rethrows that
  very object) are roots in their own right;
- values buffered inside a channel are traced through the worklist's class
  arm (`Channel<T>` is a builtin class with a handle field; buffered
  elements are marked with the object).

`thread.spawn` passes through a safepoint check right after minting the
root frame and the `Thread` object, so the spawned context's frames are
covered by the root set from birth. For the concurrency semantics, see
[Concurrency](../language-spec/concurrency.md) and the
[Frame Machine and Scheduler](frame-machine-and-scheduler.md) page.

### Array Field and Element Tracing

- **Array-typed fields are declared, not inferred**: array struct/class
  fields store `RTK_Array` as their `fieldTypeKinds` entry in the
  `.ncu`. MarkPhase routes class and struct
  field references with an explicit declared-kind + runtime-slot-kind
  double condition — `fieldTypeKinds[i] == RTK_Array` paired with the
  slot actually holding an array record — alongside the existing
  RTK_Class/RTK_Struct/RTK_Func arms.
- **Field tracing keys on the declared kind**: MarkPhase does not trace
  a field slot just because its runtime kind looks like an array. This
  is safe because jagged declarations (`T[][]`), the one source form
  that could smuggle an array record into a non-array-typed field slot,
  are rejected at resolve time by the compiler.
- **Defensive RTK_Array element arm**: the array branch traces elements
  whose declared `elemKind` is `RTK_Array` (same double condition as
  the field arms). The arm is unreachable from compilable source (jagged
  declarations are rejected) — it is the correctness base for
  externally produced `.ncu` paths. Array-typed *container*
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
Function entry (frame push) → CheckGCSafepoint()
Loop back-edge (OP_Jump backward) → CheckGCSafepoint()
Thread intrinsic (spawn, after minting the context) → CheckGCSafepoint()
                            ↓
CheckGCSafepoint → if pending && (heap > threshold || strings > strThreshold)
                   → CollectGarbage()
```
