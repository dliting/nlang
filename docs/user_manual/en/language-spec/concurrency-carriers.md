# Carriers and Parallelism

## The execution model: virtual threads and carriers

NLang's unit of concurrency is the **virtual thread** — the task you start with `thread.spawn`. Virtual threads are lightweight; a program can hold thousands of them at once, and they are not operating system threads. What actually runs virtual threads are **carriers**: operating system threads that carry virtual threads. The library defaults to a single carrier; `nvm` to at most four (`--carriers=N`, 1..64); embedding hosts opt in with `VmExecutor::SetCarrierThreadCount`. Many virtual threads time-share a few carriers — the same family of model as Go's goroutines and Java's virtual threads.

A virtual thread **suspends** at a wait point: its full execution state freezes, it yields its carrier, and after being woken it continues in place while the carrier runs other virtual threads in the meantime. The distinction from **blocking**: what suspends is the virtual thread while the carrier keeps working; blocking means the carrier thread itself waits inside an operating system call (a file read, say). Native calls declared blocking arrange for the first kind of waiting — see below.

## Is there a global lock

NLang has no whole-machine global interpreter lock (GIL — if you know Python, the lock in that sense does not exist here). Storage protection is grouped by **data category**, one lock per category: one for containers (List/Dict), one for synchronization primitives (mutex/channel), one for streams, one for string maintenance. Same-category operations serialize against each other; different categories proceed simultaneously; heap and string **allocation** proceeds per carrier without a lock; object **field reads and writes** take no lock. The internal structure of a container or a string can therefore never be torn by concurrency.

The unlocked categories carry a cost: once an object is shared across carriers (through `spawn` arguments or a channel), concurrent reads and writes of its fields are a **data race** — a half-written transitional value may be read, but memory is never corrupted. Coordinate shared mutable state with a `Mutex`.

## Which code runs truly in parallel

Under a single carrier everything serializes (virtual threads merely time-share). With multiple carriers, what runs at the same time:

- **pure-compute segments** — code touching only local variables: arithmetic, comparisons, branches, local assignments;
- **object-dense code** — object construction and field access take no lock;
- **store operations in different categories** — a container operation does not wait on a stream operation; only same-category operations queue.

Programs that spend their time inside **blocking native calls** (file and network input/output) benefit as well: such calls carry a blocking declaration in their native signature; while the call runs, the calling virtual thread suspends and all its locks are released, so the remaining carriers keep running other virtual threads (at most as many native calls are in flight at once as there are carriers; beyond that they queue). Direct native calls that touch no shared storage (math functions, say) neither wait nor hold locks. Host-registered callbacks always run under the blocking protocol, so their waits occupy no carrier. How to write the blocking declaration: see [Native functions](native-functions.md) and [Developing native libraries](../libraries/developing-libraries.md).

## Determinism and scheduling

- **A single carrier (the library default) is fully deterministic**: a strict first-in-first-out (FIFO) rotation where each virtual thread runs about 1000 loop iterations before yielding (and only when another runnable virtual thread exists); the same program with the same input produces the same interleaving. Single-carrier tests may assert interleaving and final values.
- **Multiple carriers: order is unspecified**: the scheduler may take runnable virtual threads in any order (preferring the one it last ran, for cache locality), and changing the carrier count changes the interleaving. Program correctness never rests on the interleaving — only on the `join`, channel, and mutex happens-before edges. Multi-carrier tests assert results after synchronization, never order or durations.

A virtual thread switches only at a **safepoint** — a position where pausing is safe: function entries and loop-iteration boundaries. Garbage collection likewise waits for every carrier to reach a safepoint; programs cannot observe the pause. `sleep` is a scheduling hint with a lower bound: when every virtual thread is waiting, sleepers wake within the operating system's timer granularity (about 15.6 ms on Windows). A paused debugger blocks new virtual threads from starting while the stop lasts; each in-flight segment stops once it finishes (see [Known limitations](known-limitations.md)).

The defaults stay conservative: leave the carrier count alone unless measurement shows a benefit.
