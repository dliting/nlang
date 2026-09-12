# Memory Management


### Heap Layout

All struct and class objects share a single heap (`m_structHeap`). Each heap
slot is a `vector<int32>`:

- **Struct slot**: `[field0, field1, ...]` (no type header)
- **Class slot**: `[classIdx, field0, field1, ...]` (slot[0] = runtime type ID)

Heap index 0 is a sentinel (null/invalid). Valid indices start at 1.

### Garbage Collection

NLang uses a mark-sweep garbage collector for heap records (class,
struct, array, and function values):

1. **Trigger**: GC runs at safepoints when `m_gcPending` is set and heap size
   exceeds the threshold. Safepoints are function entry and loop back-edges.
2. **Mark phase**: Precise scan via `LocalDescriptor` — local slots (and
   the pending return slot) whose declared kind is a reference kind —
   class, struct, array, or function record — are scanned; primitives
   never are. No conservative byte-scanning (decoupled from stack frame
   physical layout). Marking uses an iterative worklist (not recursive)
   to avoid stack overflow on deep object chains.
3. **Sweep phase**: Unmarked reference records (class, struct, array,
   function) are freed. Class-owned struct fields (value semantics) are
   freed with their owning class. Class fields (reference semantics) are
   freed independently by GC if unreachable.
4. **Free list**: Swept slots are added to a free list. New allocations
   prioritize reuse of free slots.

**Design decisions** (documented in VmExecutor.h):
- Safepoint-triggered, not allocation-point-triggered (avoids tracking
  tempSlot/tempSlot2 in MarkPhase)
- Precise scan via LocalDescriptor, not conservative byte scan (decouples
  GC from stack frame physical layout)
- `m_slotStructIdx` parallel array for struct type identification (structs
  have no type header; smaller change than adding one)

### Struct Lifetime

Struct lifetimes are owner-tied (value semantics):
- Local struct variables: live until the function returns (or the variable is
  reassigned, at which point the old struct's owned nested structs are freed
  by deep-copy logic)
- Class-owned struct fields: freed when the owning class object is swept by GC
- Struct-owned nested structs: freed recursively with their parent
