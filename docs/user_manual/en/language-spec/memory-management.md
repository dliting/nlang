# Memory Management

NLang is a garbage-collected language: there is no `delete` and no free function to call — objects are reclaimed automatically by the garbage collector once unreachable. You never release memory manually.

### When collection runs

Collection runs at safepoints — function entries and loop-iteration boundaries — when a collection request is pending and either the heap size or the string-object population exceeds its threshold. Collection never moves objects: an object keeps its heap index for life; sweeping only marks unreachable objects as free, to be reused by new allocations.

### What counts as reachable

The collector marks everything reachable from a set of roots; anything unmarked is reclaimed. The roots include:

- each function's local variables and the return value currently being evaluated;
- fields of classes and structs, array and `List`/`Dict` elements, string fields and string array elements;
- the call stacks of suspended virtual threads, and elements sitting in channel buffers;
- references the host program holds through the embedding interface.

Structs are freed with their owner (value semantics): a local struct lives until its function returns; a class-owned struct field is reclaimed with its owning class; nested structs are freed recursively with their parent. String literals are immortal module constants and are never collected.

The full mechanics of the collector (how marking works, the free list, the stop-the-world handshake of concurrent collection) live in [garbage collector design](../vm-architecture/garbage-collection-design.md).
