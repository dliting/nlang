# Garbage Collection Design

NLang's heap is managed automatically by garbage collection (GC):
developers create objects without freeing them by hand. This page
records the four core design decisions — safepoint triggering, a
precise per-slot handle bitmap with a conservative fallback, the
parallel array, iterative marking — plus the full mark-sweep algorithm,
the root set for parked contexts and in-flight blocking natives, and
the tracing rules for array fields. Understanding these decisions tells
you when the VM collects and which slots count as live.

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
references in the temp slots; with the handle bitmap (decision 2) covering
staging slots at every safepoint, that argument no longer carries weight —
the lasting value of safepoint triggering is a deterministic, cost-bounded
moment for the mark work.

**2. Precise root class per slot from a serialized bitmap**

A conservative byte scan of the whole frame (testing every aligned value as
a candidate heap index) was the rejected option: it couples GC to the
physical stack frame layout (slot width, alignment, tempSlot offsets), and
if the layout changes, GC breaks silently. Slots therefore carry semantic
information instead. Parameters stay on the declared route
(`LocalDescriptor.typeKind` identifies reference slots,
`LocalDescriptor.offset` locates them). Every other slot in the band
`[paramCount, localsSize)` — the return slot, the temp pool, the
call-argument staging area, the evaluation scratch area, and the named
local variables — is classified by a per-function **handle bitmap**
serialized with the function record (NCU format 2.4): two bits per slot,
one of four classes.

- `None`: a scalar; the slot never holds a reference and is not tested.
- `StrHandle`: a string-store handle; only the string domain is tested.
- `HeapIndex`: a struct/class/array/function heap index; only the heap
  domain is tested.
- `Dual`: unknown; both domains are value-tested — exactly the
  conservative test, kept as the fallback class.

The backend derives the classes from the frame layout: the fixed scratch
region below the evaluation area (temps and call staging) is `Dual`,
because any expression may pass through it; a named local and the return
slot take their declared kind; an evaluation-area slot starts as `None`
and widens by the claims of the code sites that use it
(`EvalAreaClaim`). A claim may name a narrow class only when the site can
enumerate every value that passes through the slot — the delegated-call
claim is `HeapIndex`, for example, because a single `OP_LoadField` writes
that slot. A claim over a slot that also receives expression
intermediates stays `Dual`; a slot claimed by several sites takes the
union of their classes (bitwise OR), which can only widen toward `Dual`.

Why this matters: the declaration table cannot see staging slots. In
`f(g(), h())`, g's return value lives only in the caller's evaluation
scratch area when h's frame is pushed (a safepoint). The bitmap closes
that gap with less work per collection and no dependence on physical
layout: only `Dual` slots pay the two-domain value test. Frames are
zero-initialized, so an unwritten cell holds 0 and hits neither domain;
the collector never moves objects, so a cross-domain hit in a `Dual` slot
can only over-mark (an object lingers briefly), never under-mark. A
hand-built in-memory function record whose bitmap length disagrees with
its frame layout (the loader rejects that shape in artifacts) falls back
to the conservative both-domain scan of the whole band.

A parked context's frames can be frozen mid-instruction, and their staging
values must survive exactly the same way — the bitmap serves parked frames
and running frames alike (see "Parked contexts and the scheduler root
set"), with no separate root machinery for the parked shape.

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
    //parameters: declared descriptors
    for each LocalDescriptor with isParam and a reference typeKind:
      RTK_String: MarkString(value)      //handle, not heap index
      otherwise:  if valid and not marked: set mark bit, push to worklist
    //return slot: declared return kind
    if returnTypeKind is a reference kind:
      RTK_String: MarkString(value)      //conservative over-mark, as defense
      otherwise:  mark and push if valid
    //band: two-bit root class per slot (NCU 2.4 handle bitmap)
    for each band slot i, slot number paramCount + i:
      class = (func.evalAreaHandleMask[i >> 2] >> ((i & 3) * 2)) & 3
      None:      skip
      StrHandle: MarkString(value)
      HeapIndex: mark and push if valid
      Dual:      value-test both domains
    //fallback for a bitmap whose length disagrees with the frame layout:
    //value-test both domains over the whole band
  for each scheduler context:      //parked/queued contexts (steal model)
    for each parked CallFrame: run the same three arms (params, result, band)
    if threadObjHeapIdx or failExcHeapIdx valid: mark, push to worklist
  for each in-flight blocking native call:
    staged argument cells and return slot: value-test both domains
    handles minted by re-entrant callbacks: value-test both domains
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
  running frame (parameter descriptors, result cell, band bitmap) — a park
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

### GC Under the GIL and Blocking-Native Roots

Every mutation of the heap — allocation, marking, sweeping — happens
while holding the global interpreter lock (GIL; see
[Concurrency](../language-spec/concurrency.md)). A collection runs on the
carrier that holds the GIL, and no other carrier can touch the heap
meanwhile: there is no separate stop-the-world handshake to coordinate.

- A non-blocking native call holds the GIL for its whole duration, so no
  collection can interleave with it and its arguments need no extra
  rooting.
- A **blocking** native call — declared through the version 5
  registration channel of the native application binary interface
  (ABI) — uses staged cells: the VM copies the arguments into cells it
  owns, releases the GIL, calls the native, re-acquires the GIL (checking
  the stop flag first), restores the frame, and copies the result back.
  During the window without the GIL, a collection on another carrier can
  run, so the staged argument cells and the return slot are roots for the
  duration of the call. The return cell is a root by address — the native
  writes it itself.
- Callbacks into the VM made from inside a blocking native (string
  construction, list construction, raising an exception) re-acquire the
  GIL per callback. Handles they mint are pushed onto the executor's
  local-root vector and truncated at a watermark when the call returns;
  they live on the executor rather than on the carrier's C++ stack because
  a collector running on another carrier cannot see that stack.
- Root-vector mutations happen under the GIL (armed before the release,
  truncated after the re-acquire), so a collection always reads a
  consistent list.

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

A safepoint only fires on a carrier that holds the GIL; parked contexts
are not running and need no cooperation to be scanned.
