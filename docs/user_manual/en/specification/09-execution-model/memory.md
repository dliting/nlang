# 9.2 Lifetime and Garbage Collection

Objects — class instances, struct records, arrays, function
values and strings — live in an implementation-managed heap. The
language offers no deallocation operation: a program never frees
an object, and reclamation runs no user code (there is no
finalizer or destructor).

An object is *reachable* if it can be found from the roots by
following references: the live slots of active function frames
(parameters, locals, temporaries and staging slots), the fields
of reachable objects, the elements of reachable containers, the
captured receivers of live function values, and references held
by the host. The implementation makes two promises:

- a reachable object is never reclaimed;
- an unreachable object MAY be reclaimed at any time; when
  reclamation happens is *unspecified* (§0.4).

Because reclamation runs no user code and its timing is
unspecified, a program cannot observe when — or whether — an
object is reclaimed. A struct record is owned by its holder (a
local slot, a field, or an element of an array or container) and
becomes unreachable together with it.

What happens when the implementation cannot obtain memory for a
new object is *unspecified*.

### When collection runs

*Informative:* NLang is a garbage-collected language: there is no
`delete` and no free function to call — objects are reclaimed
automatically once they are unreachable, and you never release
memory manually.

Collection runs at safepoints — function entries and loop-iteration
boundaries — when a collection request is pending and either the heap
size or the string-object population exceeds its threshold. Collection
never moves objects: an object keeps its heap index for life; sweeping
only marks unreachable objects as free, to be reused by new allocations.

### What counts as reachable

*Informative:* the collector marks everything reachable from the roots;
anything unmarked is reclaimed. The roots include:

- each function's local variables and the return value currently being
  evaluated;
- fields of classes and structs, array and `List`/`Dict` elements, string
  fields and string array elements;
- the call stacks of suspended virtual threads, and elements sitting in
  channel buffers (chapter 10);
- the static slots of loaded modules — class and interface statics,
  static constants and static locals (§4.8.8, §6.1) — including the
  zero-value struct records materialized for initializer-less struct
  statics (§4.7.4);
- references the host program holds through the embedding interface
  (see [Embedding NLang](../../libraries/embedding-api.md)).

Structs are freed with their owner (value semantics): a local struct
lives until its function returns; a class-owned struct field is
reclaimed with its owning class; nested structs are freed recursively
with their parent. String literals are immortal module constants and are
never collected.

The full mechanics of the collector — how marking works, the free list,
the stop-the-world handshake of concurrent collection — live in
[garbage collector design](../../vm-architecture/garbage-collection-design.md).
