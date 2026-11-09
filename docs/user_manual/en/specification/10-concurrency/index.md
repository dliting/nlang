# 10. Concurrency

NLang runs concurrent work as *virtual threads*: lightweight
execution contexts scheduled onto a small pool of *carrier*
threads. This chapter fixes the execution model and the contract of
each concurrency name: the contexts and the `thread` operations
(§10.1), the locks and counters (§10.2, §10.3, §10.6), message
passing (§10.4), result handles (§10.5) and the permitted
interleavings (§10.7) are on this page; carrier configuration and
parallelism (§10.8) and suspension and scheduling limits (§10.9)
each have their own page.

## 10.1 Threads and Carriers

NLang concurrency is built on *virtual threads*: lightweight
execution contexts scheduled by the implementation onto *carrier*
threads, which are operating-system threads. A program's contexts
share one heap (§9.2): every context sees the same objects. A
context gives up its carrier only at a safepoint — function entry
or a loop back edge — and each context runs at most 1000 loop back
edges before another may be dispatched.

The concurrency names live in two standard-library packages
(chapter 18), each imported by name (§3.9.4): `thread` — declared
in `stdlib/thread.n` — answers who runs and where the result is
fetched (`Thread`, `Future<T>`, `Promise<T>`, and the `thread.*`
functions); `concurrent` — declared in `stdlib/concurrent.n` —
answers how running computations coordinate (`Mutex`,
`ReentrantMutex`, `Semaphore`, `AtomicInt`, `Channel<T>`,
`ChannelClosedException`, `WaitGroup`). Their declarations live in
the standard library and their implementations are built into the
virtual machine.

- `thread.spawn(f)` takes a zero-argument function value (§7.4):
  a free function, a bound method, or a variable holding one. It
  starts a new context and returns its handle immediately. A
  delegate of type `func<void>` yields a `Thread` handle; a
  delegate of type `func<T>` yields a `Future<T>` handle (§10.5).
- `thread.join(t)` suspends the calling context until the target
  context finishes. Several contexts may join one target; all are
  woken. Joining an already-finished context returns immediately;
  joining a context onto itself is a *host-level* run-time error
  (§8.1).
- `thread.isFailed(t)` never parks: it yields `true` once the
  target has finished with an uncaught exception — `false` while
  it runs and after a clean finish. `thread.failure(t)` yields
  the failed target's original exception object, the same
  instance `join` rethrows, or `null` while the target has not
  failed; the read itself counts as observing the failure, so
  the run-end report of §10.1 does not fire for it.
- `thread.yield()` moves the calling context to the back of the
  run queue.
- `thread.id()` yields the current context's identifier: the
  entry context is `0`, spawned contexts are non-zero.
- `thread.sleep(ms)` suspends until the deadline; `sleep(0)` is a
  yield; a negative duration is a *host-level* run-time error
  (§8.1). The deadline is a lower bound: a sleeper wakes within
  the host's timer granularity, and sleepers wake in deadline
  order.

An exception that escapes the top of a spawned context's body
marks the context *failed*; it propagates neither to the host nor
to the entry context. The failure surfaces through `join` — or
through `get()` on the context's `Future` (§10.5) — as the
original exception object, with its class, `message` and
`backtrace` preserved (§8.3). A failure no context ever observes
is not lost: at run end the implementation reports it on the error
output, one line per unobserved failed context.

A *carrier* is an operating-system thread that runs contexts. The
library default is one carrier; `nvm` starts at most four carriers
and accepts `--carriers=N` for `N` from 1 to 64; an embedding host
opts in with `VmExecutor::SetCarrierThreadCount`. Execution is not
serialized by a global lock: storage protection is grouped by
*data category*, one lock per category — containers (`List`,
`Dict`), synchronization primitives (mutex, semaphore, channel),
streams, and string maintenance. Operations in one category
serialize against each other; different categories proceed at the
same time, and heap and string allocation needs no global lock.
Object field reads and writes take no lock, so once an object is
shared across carriers — through a `spawn` argument or a channel —
concurrent reads and writes of its fields are a *data race*: a
half-written transitional value may be read, but memory is never
corrupted. The internal structure of a container or a string is
therefore never torn by concurrency, and shared mutable state is
coordinated with a `Mutex` (§10.2). A native call that declares a
blocking section (§27.7) releases the locks it holds while it
waits, so other contexts run; a native that declares none runs
without holding them, and its stores take the category locks
through self-guarded callbacks.

When the entry context returns, the program ends: contexts still
parked or queued are dropped without running further. If no
context is runnable, no timer is pending, and no native call is
blocked, the implementation reports a deadlock as a *host-level*
run-time error (§8.1) rather than waiting forever.

Suspending operations — `join` on an unfinished target,
`Future.get()` on an unfinished target, a contended `lock`, an
empty-channel `recv`, a full-channel `send`, `sleep` and `yield` —
freeze the context's full state: frames, locals, handler state
(§8.2) and instruction position. A `throw` after a parked `try`
body still reaches that body's `catch`; a `finally` still runs
exactly once; a rethrow after a parked `catch` still rethrows
(§8.1). These rules hold inside a nested evaluation window too —
a `toString` evaluation, or a host callback firing mid-run: the
window suspends, other contexts run while it waits, and it
resumes in place. The one exception is a host `call()` window
with no carrier in flight (outside a run): there the suspending
operations are rejected as *host-level* run-time errors (§8.1),
and `thread.spawn` is rejected the same way — no carrier exists
to schedule anything while the window waits.

## 10.2 Mutex and ReentrantMutex

A `Mutex` is a non-reentrant mutual-exclusion lock. It is a
*handle class*: a class whose instances wrap an
implementation-managed object and whose constructors and methods
are built into the virtual machine. The concurrency handle
classes are declared by their packages (§3.9.4) — `Mutex` by
`concurrent`, `Thread` and `Future<T>` by `thread` — so their
names reach scope through the package import, like any other
package class.

- `lock()` acquires the lock. While another context holds it, the
  caller parks in a queue and is woken in arrival order. Locking a
  mutex the caller already holds parks forever: nothing can wake
  the caller, and the program ends under the exit policy of §10.1
  — the deadlock report of §10.1 when nothing else can make
  progress, otherwise the parked context is dropped when the
  entry context returns.
- `unlock()` releases it. Unlocking a mutex the caller does not
  hold is a *host-level* run-time error (§8.1).
- `tryLock()` never parks: it acquires and yields `true` when the
  lock is free, and yields `false` when it is held.

Holding a mutex across `yield` or `sleep` is legal: the
mutual-exclusion guarantee does not lapse while the holder is
away.

A `ReentrantMutex` (also `concurrent`) is the reentrant form of
the same contract: the context that holds the lock may `lock()`
again without parking, an owner count accumulates, and `unlock()`
releases the lock only when the count reaches zero. Between two
contexts it behaves exactly like a `Mutex`. It exists for classes
whose methods acquire the lock and call each other; code without
that shape takes the plain `Mutex`.

## 10.3 Semaphore

A `Semaphore` admits at most N contexts into a section: a mutex
admits one, a semaphore admits N. It is a handle class of the
`concurrent` standard-library package (chapter 18; import
required, §3.9.4):

- `Semaphore(n)` sets the initial permit count. A negative `n`
  is a *host-level* run-time error (§8.1). `0` is legal — the
  signal-switch shape, where waiters stop at `acquire` until a
  later `release` lets them through.
- `acquire()` takes one permit. While no permit is available the
  caller parks in a queue; `release()` hands the permit directly
  to the front waiter, in arrival order.
- `release()` returns one permit. There is no ownership
  requirement: `release` without a matching `acquire` is legal
  and the count may exceed the initial value.
- `tryAcquire()` never parks: it takes a permit and yields
  `true` when one is available, and yields `false` otherwise.

The `concurrent` standard-library package (chapter 18) also
provides `WaitGroup`: `add(n)` accumulates outstanding units,
`done()` retires one, and `wait()` parks until the count reaches
zero. The contract is `add` before `wait` and `done` paired with
`add`: an unmatched `done` releases `wait` early, and a missing
`done` parks it forever — the program ends under the exit policy
of §10.1. Unlike the handle classes, `WaitGroup` is ordinary
NLang code declared in the package source and built on `Mutex`
and `Channel`.

## 10.4 Channel

A `Channel<T>` passes values between contexts. The element type
argument is required: it drives the boxing on send and the
unboxing on receive, by the same mechanism as `List<T>` (§4.12);
reference elements pass through directly. A channel is a handle
class of `concurrent` (§10.2).

Construction takes two forms: `{}` creates an unbounded channel;
`Channel(n)` creates one with buffer capacity `n` — `0` means
unbounded, `n >= 1` bounds the buffer, and a negative `n` raises
`Exception` (§8.3).

- `send(v)` appends to the buffer. On an unbounded channel it
  never parks; on a bounded channel it parks in a queue while the
  buffer is full and resumes in arrival order as slots free.
- `recv()` takes the head value when the buffer is non-empty;
  otherwise it parks until a `send` or a `close`.
- `close()` closes the channel; it is idempotent, and values
  already in the buffer are kept.

Closing is observed through the catchable
`ChannelClosedException` (§8.3). After `close`, `recv` still
delivers the buffered values; once the buffer is drained, a
further `recv` raises the exception, and any `send` after the
close raises it at once. Parked waiters see the same boundary:
`close` first drains the buffer into the parked receivers, then
wakes the remaining receivers and every parked sender with the
exception.

## 10.5 Future and Promise

A `Future<T>` is the retrieval handle for a computation's return
value — produced by `thread.spawn` on a `func<T>` delegate
(§10.1), or by a `Promise`. It is a handle class of `thread`
(§10.2).

- `get()` parks until the target finishes, then yields its return
  value typed `T`. If the target failed, `get()` rethrows the
  original exception object (§8.3). Several contexts may `get()`
  one future; all wake with the same value or the same object.
  `get()` on a finished future returns immediately and may be
  called repeatedly. A context may not `get()` the future of its
  own spawn: that call is a *host-level* run-time error (§8.1),
  since nothing could wake the context.
- `isDone()` never parks: it yields `true` once the target has
  finished, normally or failed.
- `Future.selectAny(futures)` takes an array of `Future` values,
  parks until any one member decides and yields the deciding
  member's index — the least index when several are already
  decided. A failed member decides too: the winning index
  carries no exception; observing a failure is `get()`'s job on
  the winner. The other members run on untouched. An empty
  array, a `null` member, more than 64 members and a member
  that is the calling context's own spawn are *host-level*
  run-time errors (§8.1).

A `Promise<T>` (also `thread`) is the user-constructed completion
source — the counterpart of a spawn-minted future, for a value
one context will compute and others will await.
`promise.future()` mints a retrieval handle: repeatable, and
every mint shares the one decision. `complete(v)` decides the
outcome with value `v`; `completeExceptionally(e)` decides it
with the exception object `e`. Exactly one call decides — the
first wins, later calls yield `false` without effect — and every
waiter wakes with the value, or has `e` rethrown into it by
`get()`.

## 10.6 AtomicInt

An `AtomicInt` (handle class of `concurrent`) wraps one integer
whose operations are atomic: each read, decision and write form
one indivisible step, so contexts on different carriers never
interleave inside an operation.

- `AtomicInt(initial)` sets the initial value; `AtomicInt()` is
  zero.
- `load()` yields the value; `store(v)` replaces it.
- `getAndSet(v)` replaces the value and yields the previous one.
- `compareExchange(expect, v)` stores `v` only when the current
  value equals `expect`, yielding whether the store happened.
- `getAndAdd(delta)` adds and yields the previous value;
  `addAndGet(delta)` adds and yields the new one;
  `incrementAndGet()` and `getAndIncrement()` are the `delta` of
  one, yielding the new and the previous value respectively.

The naming follows Java's `AtomicInteger`: `getAndX` yields the
value before the operation, `xAndGet` the value after. Atomics
cover one value: state made of several fields, or any
read-decide-write whose decision spans more than one call,
belongs behind a `Mutex` (§10.2).

## 10.7 Permitted Interleavings

Under a single carrier the schedule is deterministic: a strict
first-in-first-out queue with a fixed back-edge budget (1000), so
the same program with the same inputs produces the same
interleaving on every run, apart from `sleep` deadlines, which
follow the host's clock (§10.1).

Under more carriers, dispatch may reorder: a carrier prefers the
context it last ran, so which context leaves the queue first is
*unspecified* (§0.4), and changing the carrier count changes the
interleaving. What remains guaranteed is the result after
synchronization: `join`, `Future.get()`, a channel handoff and a
mutex release-to-acquire each form a *happens-before* edge —
every write the earlier context made before the edge is visible
to the later context after it. A program's correctness rests on
these edges, never on the interleaving; multi-carrier programs
assert results after synchronization, never interleaving or
durations.

### The concurrency model: blocking style, no async coloring

*Informative:* NLang belongs to the **virtual-thread** family (like
Go, and like Java's virtual threads): writing concurrent code reads
like writing ordinary sequential code — when a wait happens, the
context steps aside, and the scheduler interleaves the contexts
between suspension points. Under this model, "single-threaded async"
is the default shape: the library defaults to one carrier, and the
contexts spawned by `thread.spawn` take turns on that single line of
execution — waiting on timers, channels, and each other — solving the
same problem as a JavaScript event loop, just written as blocking,
straight-line code:

| Single-threaded async (JavaScript) | The NLang equivalent |
|---|---|
| event loop | the scheduler's run queue (one carrier by default) |
| `setTimeout(f, 100)` | `thread.spawn(f)` plus `thread.sleep(100)` |
| `await promise` / `Promise.all` | `f.get()` / `thread.join(t)` |
| callback nesting | plain sequential code |

This is why NLang has no `await` keyword and does not need one:
functions are not async-colored and nothing compiles into a state
machine; `f.get()` is an honest blocking wait — the current context
suspends, and once the value is ready it continues in place. To run on
several carriers with real parallelism (pure-compute segments and
object field access overlap directly; containers, strings, streams,
and synchronization primitives each take their own lock, one per data
category), opt in with `SetCarrierThreadCount(n)` (see [Carriers and
Parallelism](carriers.md)).

### Choosing the right tool

*Informative:* start from the task you have, not from the tool list. The
shipped `examples/conc_*.n` walk through each row:

| You want to… | Use |
|---|---|
| start a background task | `thread.spawn(f)` |
| get a task's return value | spawn a returning function → `Future<T>` → `get()` |
| construct and complete a `Future` by hand | `Promise<T>` (`future()` hands out the handle, `complete` is first-wins) |
| wait for whichever is ready first | `Future.selectAny(futures)` (returns the winning index) |
| hand values between tasks | `Channel<T>` (send/recv/close) |
| guard shared mutable state | `Mutex` (lock/unlock/tryLock) |
| lock inside composed or recursive methods | `ReentrantMutex` (reentrant; see [ReentrantMutex](../library/18-concurrency-classes/reentrant-mutex.md)) |
| atomic updates to a single-value counter or flag | `AtomicInt` (load/store/compareExchange; see [AtomicInt](../library/18-concurrency-classes/atomic.md)) |
| cap how many tasks run a section at once | `Semaphore(n)` |
| wait for a batch of tasks | `WaitGroup` (add/done/wait) or `join` each |
| wake a group of waiters at once | `Channel.close()` broadcast |

### Where to look

- [Carriers and Parallelism](carriers.md) — the execution model, storage
  protection, which code runs truly in parallel, determinism, and
  choosing the carrier count
- [Suspension and scheduling limits](suspension.md) — suspension versus
  blocking, exceptions across suspension, the main-exit policy, the
  deadlock report, suspension inside re-entrant evaluation
- [The Concurrency Classes](../library/18-concurrency-classes/index.md) —
  the library chapter: package form, plus one page per class
- [The thread package](../library/18-concurrency-classes/thread.md) —
  `spawn`, `join`, `yield`, `id`, `sleep`
- [Future](../library/18-concurrency-classes/future.md) — retrieving a
  spawned task's return value
- [Promise](../library/18-concurrency-classes/promise.md) — the manual
  completion source, and `Future.selectAny` multi-way waiting
- [Mutex](../library/18-concurrency-classes/mutex.md) — non-reentrant
  mutual exclusion
- [Semaphore](../library/18-concurrency-classes/semaphore.md) — a counting
  semaphore
- [Channel](../library/18-concurrency-classes/channel.md) — message passing
  and closing semantics
- [The concurrent package](../library/18-concurrency-classes/concurrent.md)
  — the package's classes, including `WaitGroup`
