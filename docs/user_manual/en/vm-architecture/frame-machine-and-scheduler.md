# Frame machine and scheduler

The virtual machine's interpreter is a **frame-stack machine**: NLang function calls no longer rely on C++ recursion. Calls live on an explicit frame stack, and one iterative loop dispatches opcodes. This restructuring is the foundation of concurrency — suspending a running context requires freezing its entire call stack into data, and a recursive interpreter buries the call stack in the C++ stack, where that is impossible.

## The frame machine

Each NLang function call pushes a call frame; `OP_Return` pops it and writes back the result. The dispatch loop keeps interpreting the top frame's instructions, so NLang call depth no longer consumes C++ stack. The nesting-depth limit (1000 frames) keeps its semantics and becomes a frame count. Two bounded exceptions remain C++-recursive: the re-entrant evaluation for `toString` (depth limit 64) and the embedding host's `call()` entry point (which cannot nest further).

A call frame (see the [stack frame layout](stack-frame-layout.md)) carries:

- the local-variable buffer with receiver and parameter slots;
- the return destination (the caller's slot offset, or an external cell provided by the host);
- the resumption point (`pc`) and the source line;
- the exception-handler state (the stack of currently entered `catch` handlers) — it belongs to the frame, so it naturally survives suspension.

## The suspension mechanism

Suspension introduces no new opcodes. When a thread intrinsic needs to yield, it throws a virtual-machine-private suspension signal (a C++ exception that never travels the NLang exception path). The loop layer catches it and moves the frame stack, untouched, into the context's suspended record. Resuming means moving the frames back onto the call stack and continuing from each frame's recorded resumption point — local variables, handler state, and instruction position are all exactly as they were at the suspend.

Implementations of the intrinsics follow a "complete or suspend" rule: an intrinsic either finishes its operation entirely, or suspends before making any observable change. There is no half-done suspend.

Preemption reuses the same mechanism: when the back-edge counter reaches its quota (the constant `DEFAULT_SCHEDULER_TRIP_BACKEDGES`) and another runnable context exists, a preemption signal is thrown and the carrier requeues the current context at the tail of the run queue. **A single-context program is therefore bit-for-bit unchanged** — the quota never fires when there is no one else to run.

Nested evaluation windows (`toString` evaluation, the embedding host's `call()`) may not suspend: a suspension request there is converted into a catchable runtime error, ahead of every existing fallback handler. This keeps the window's calling convention (call returns a value) intact.

## The scheduler

The scheduler keeps one record per context: identifier, state (runnable; suspended on a timer, a join, a mutex, a channel receive, or a full-channel send; finished; failed), the suspended frame stack, heap references for the `Thread` object and the failure exception, the wake-up time, and the completion result value.

- **Run queue**: first-in-first-out, with **wake affinity** on dequeue —
  a carrier scans a constant window at the head of the queue (8 entries)
  and prefers the context it last ran itself (a never-dispatched context
  counts as matching any carrier); if the window holds no match, the
  head dispatches. Under a single carrier every entry matches, so the
  head is always taken and the queue degenerates to strict FIFO.
  Carriers are numbered (the primary carrier is 0, auxiliary carriers
  are 1..N); the scheduler keeps a lock-held registry of which context
  each carrier currently holds, serving debug enumeration and the
  affinity stamp. The carrier loop repeatedly takes a context and runs
  it until it returns, suspends, or is preempted.
- **Timer list**: `thread.sleep` suspensions are ordered by wake-up time; a dedicated timer thread (started on the first sleep) moves due sleepers back to the run queue, so a deadline is honored even when every carrier is suspended or blocked in a native call.
- **Context lifecycle**: `thread.spawn` mints the target context's root frame at spawn time (the delegate's receiver goes into frame slot 0), so the garbage collector's precise root set covers the context from birth. A finishing context wakes all its joiners; an uncaught exception marks it failed with the exception object retained for `join` to rethrow — the failure never leaks to the host.
- **Exit policy**: when the main context returns, the whole run ends and every other context is dropped silently. If the queue drains while main is still unfinished, the virtual machine reports a deadlock instead of spinning.

## Where the garbage collector plugs in

Suspended frame stacks are part of the root set: parameters and the result slot are scanned through the declaration table, and every other frame slot — the evaluation staging area included — is scanned through the per-function handle bitmap. Mechanism and motivation are covered in the [garbage collector design](garbage-collection-design.md).

## Debugger integration

Debug hooks (statement callbacks, breakpoints, throw triggers) fire **for every context**: a breakpoint hit, a step, or a thrown exception in any context stops the run, and the stop is all-stop — a freeze gate blocks every new dispatch, so each in-flight dispatch segment finishes and then the carrier halts; a timer that falls due during the pause may already be queued, but everything waits for the resume before it dispatches (time-frozen semantics). Statement stepping targets the **debug focus** context (`ndb` lists contexts with `threads` and switches focus with `t <id>`); stepping does not follow `thread.spawn` — a spawned context enters view through its own breakpoint hits. While stopped, a suspended context inspects in full — frames and locals are enumerable and readable; a context still held by a carrier keeps its frames on that carrier's execution stack and reports none until it suspends or finishes. Session details and the machine protocol are covered in the [debugger design](debugging.md).
