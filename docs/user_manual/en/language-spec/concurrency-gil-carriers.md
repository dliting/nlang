# The GIL and Multiple Carriers

## What this means for your program

Multiple carriers pay off in exactly one situation: the program spends
time inside blocking native calls (file and network input/output
through natives that declare their blocking section), where one task's
wait overlaps another task's work. A program of pure language code
gains nothing from extra carriers — bytecode executes one virtual
thread at a time regardless. That is why the defaults are conservative:
the embedding library runs a single carrier, and `nvm` uses at most
four. Leave the carrier count alone unless measurement shows a benefit.

## How it works

The VM holds a **global interpreter lock** (GIL): exactly one virtual thread executes bytecode at a time. Context switches happen only at safepoints (function entry and loop back edges), bounded by a scheduler budget of about 1000 back edges per turn. The GIL makes bytecode execution mutually exclusive, so language code never has to guard shared memory against a second virtual thread.

Garbage collection also stops the world at these safepoints: every executing window reaches one, the last to arrive runs the collection, and execution then resumes. Programs cannot observe the pause.

A **carrier** is an operating system thread that runs virtual threads. The library defaults to a single carrier; embedding hosts opt in with `VmExecutor::SetCarrierThreadCount`, and `nvm` defaults to at most four (`--carriers=N`, 1..64). Multi-carrier parallelism comes from one kind of window only: a native call that declared a blocking section under version 5 of the native application binary interface (ABI) — the VM releases the GIL for its duration, so other carriers run other virtual threads (typically overlapping an input/output wait with other work), and the GIL is reacquired before the native returns. A third-party native that does not declare its blocking section holds the GIL for the whole call and every carrier stalls.

Determinism in tiers: under a single carrier, the same program with the same input produces the same interleaving (FIFO queue plus a fixed back-edge budget). With multiple carriers, dequeuing applies **wake affinity** — a carrier scans a constant window at the head of the queue and prefers the context it last ran itself (a never-dispatched context counts as matching any carrier); if the window holds no match, the head dispatches. Affinity reorders which entry dispatches, for execution locality only, so the scheduling order is unspecified and changing the carrier count changes the interleaving; program correctness never rests on the interleaving — only on the `join`, channel, and mutex happens-before edges. Multi-carrier tests assert results after synchronization, never interleaving or durations. `sleep` is a scheduling hint with a lower bound: once every carrier is parked, a sleeper wakes within the operating system's timer granularity (about 15.6 ms on Windows). A paused debugger holds the GIL and freezes every virtual thread (see [Known limitations](known-limitations.md)).
