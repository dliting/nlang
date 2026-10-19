# Memory Management

NLang is a garbage-collected language: there is no `delete` and no free
function to call — objects are reclaimed automatically by the garbage
collector once unreachable. You never release memory manually. This page
describes the heap layout and the collection mechanics.


### Heap Layout

All struct and class objects share a single heap. Each heap
slot is a sequence of 32-bit integers:

- **Struct slot**: `[field0, field1, ...]` (no type header)
- **Class slot**: `[typeId, field0, field1, ...]` (first cell holds the
  runtime type identifier (ID))

Heap index 0 is a sentinel (null/invalid). Valid indices start at 1.

### Garbage Collection

NLang uses a mark-sweep garbage collector for heap records (class,
struct, array, and function values) and for string objects. String
objects live in a separate string object store (outside that heap)
but share the same mark-sweep cycle:

1. **Trigger**: garbage collection (GC) runs at safepoints when a collection request is
   pending and either the heap size exceeds its threshold or the string
   object store size exceeds its own threshold (the string object
   store's threshold backs off to 2x the surviving population after each
   sweep). Safepoints are function entry and loop back-edges.
2. **Mark phase**: Precise scan via the local-variable descriptor (called
   `LocalDescriptor` in the source) — local slots (and
   the pending return slot) declared with a reference kind —
   class, struct, array, or function record — are scanned; string locals
   and string return slots mark string objects by handle instead;
   primitives never are. Frame staging slots the declaration table cannot
   see (nested-call arguments, the evaluation scratch area) get a
   conservative band scan: a slot whose value falls in the valid
   string-handle range or the valid heap-index range is marked as that
   kind — the collector never moves objects, so a false positive merely
   retains, it never reclaims wrongly. Class/struct string fields, string
   array elements, boxed-string contents inside containers, and
   concatenation node children are marked as string roots too; the parked
   frame stacks of virtual threads, channel buffer elements, and
   host-held references are roots as well (see the
   [garbage collector design](../vm-architecture/garbage-collection-design.md)).
   Marking uses an iterative worklist (not recursive) to avoid stack
   overflow on deep object chains.
3. **Sweep phase**: Unmarked reference records (class, struct, array,
   function) are freed. Class-owned struct fields (value semantics) are
   freed with their owning class. Class fields (reference semantics) are
   freed independently by GC if unreachable. The string object store is
   swept independently within the same collection: constant-materialized
   objects (immortal) are never collected, and a collected interned
   string's table entry is purified in the same pass.
4. **Free list**: Swept slots are added to a free list. New allocations
   prioritize reuse of free slots (the string object store keeps its own
   free list).

**Design decisions** (for the source-level correspondence, see the
comments in `VmExecutor.h`):
- Safepoint-triggered, not allocation-point-triggered (avoids tracing
  temp slots in the mark phase)
- Precise scan of declared slots via the local-variable descriptor, plus
  a conservative range test over the staging-slot band the declaration
  table cannot see (GC therefore needs no byte-level knowledge of frame
  layout, and staging values are never missed)
- A struct-type index table parallel to the heap slots identifies
  struct types (structs have no type header; smaller change than adding
  one)
- Iterative marking with an explicit worklist, not recursion (deep
  object chains would overflow the C++ call stack; memory usage is
  O(reachable objects))

### Struct Lifetime

Struct lifetimes are owner-tied (value semantics):
- Local struct variables: live until the function returns (or the variable is
  reassigned, at which point the old struct's owned nested structs are freed
  by deep-copy logic)
- Class-owned struct fields: freed when the owning class object is swept by GC
- Struct-owned nested structs: freed recursively with their parent
