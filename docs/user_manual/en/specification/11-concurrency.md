# 11. Concurrency

## 11.1 Threads and Carriers

NLang concurrency is built on *virtual threads*: lightweight
execution contexts scheduled by the implementation onto *carrier*
threads, which are operating-system threads. A program's contexts
share one heap (§10.2): every context sees the same objects.
Exactly one context executes bytecode at a time, under an
implementation-internal global lock; context switches happen only
at safepoints — function entry and loop back edges — and each
context runs a bounded number of back edges before another may be
dispatched.

The concurrency names live in two standard-library packages
(chapter 17), each imported by name (§12.4): `thread` answers who
runs and where the result is fetched; `concurrent` answers how
running computations coordinate. Their declarations live in the
standard library and their implementations are built into the
virtual machine.

- `thread.spawn(f)` takes a zero-argument function value (§7.4):
  a free function, a bound method, or a variable holding one. It
  starts a new context and returns its handle immediately. A
  delegate of type `func<void>` yields a `Thread` handle; a
  delegate of type `func<T>` yields a `Future<T>` handle (§11.5).
- `thread.join(t)` suspends the calling context until the target
  context finishes. Several contexts may join one target; all are
  woken. Joining an already-finished context returns immediately;
  joining a context onto itself is a *host-level* run-time error
  (§9.1).
- `thread.isFailed(t)` never parks: it yields `true` once the
  target has finished with an uncaught exception — `false` while
  it runs and after a clean finish. `thread.failure(t)` yields
  the failed target's original exception object, the same
  instance `join` rethrows, or `null` while the target has not
  failed; the read itself counts as observing the failure, so
  the run-end report of §11.1 does not fire for it.
- `thread.yield()` moves the calling context to the back of the
  run queue.
- `thread.id()` yields the current context's identifier: the
  entry context is `0`, spawned contexts are non-zero.
- `thread.sleep(ms)` suspends until the deadline; `sleep(0)` is a
  yield; a negative duration is a *host-level* run-time error
  (§9.1). The deadline is a lower bound: a sleeper wakes within
  the host's timer granularity.

An exception that escapes the top of a spawned context's body
marks the context *failed*; it propagates neither to the host nor
to the entry context. The failure surfaces through `join` — or
through `get()` on the context's `Future` (§11.5) — as the
original exception object, with its class, `message` and
`backtrace` preserved (§9.3). A failure no context ever observes
is not lost: at run end the implementation reports it on the error
output, one line per unobserved failed context.

A *carrier* is an operating-system thread that runs contexts. The
library default is one carrier; a host may configure more. While
one carrier runs a context, the others wait: bytecode execution
stays mutually exclusive, so language code never guards shared
memory against a concurrent context. The lock is released for one
window only — a native call that declared a blocking section under
the native application binary interface (§22.7) — and
reacquired before the native returns; a native that does not
declare its blocking section holds the lock for the whole call.

When the entry context returns, the program ends: contexts still
parked or queued are dropped without running further. If no
context is runnable, no timer is pending, and no native call is
blocked, the implementation reports a deadlock as a *host-level*
run-time error (§9.1) rather than waiting forever.

Suspending operations — `join` on an unfinished target,
`Future.get()` on an unfinished target, a contended `lock`, an
empty-channel `recv`, a full-channel `send`, `sleep` and `yield` —
freeze the context's full state: frames, locals, handler state
(§9.2) and instruction position. A `throw` after a parked `try`
body still reaches that body's `catch`; a `finally` still runs
exactly once; a rethrow after a parked `catch` still rethrows
(§9.1). These rules hold inside a nested evaluation window too —
a `toString` evaluation, or a host callback firing mid-run: the
window suspends, other contexts run while it waits, and it
resumes in place. The one exception is a host `call()` window
with no carrier in flight (outside a run): there the suspending
operations are rejected as *host-level* run-time errors (§9.1),
and `thread.spawn` is rejected the same way — no carrier exists
to schedule anything while the window waits.

## 11.2 Mutex and ReentrantMutex

A `Mutex` is a non-reentrant mutual-exclusion lock. It is a
*handle class*: a class whose instances wrap an
implementation-managed object and whose constructors and methods
are built into the virtual machine. The concurrency handle
classes are declared by their packages (§12.4) — `Mutex` by
`concurrent`, `Thread` and `Future<T>` by `thread` — so their
names reach scope through the package import, like any other
package class.

- `lock()` acquires the lock. While another context holds it, the
  caller parks in a queue and is woken in arrival order. Locking a
  mutex the caller already holds parks forever: nothing can wake
  the caller, and the program ends under the exit policy of §11.1
  — the deadlock report of §11.1 when nothing else can make
  progress, otherwise the parked context is dropped when the
  entry context returns.
- `unlock()` releases it. Unlocking a mutex the caller does not
  hold is a *host-level* run-time error (§9.1).
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

## 11.3 Semaphore

A `Semaphore` admits at most N contexts into a section: a mutex
admits one, a semaphore admits N. It is a handle class of the
`concurrent` standard-library package (chapter 17; import
required, §12.4):

- `Semaphore(n)` sets the initial permit count. A negative `n`
  is a *host-level* run-time error (§9.1). `0` is legal — the
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

The `concurrent` standard-library package (chapter 17) also
provides `WaitGroup`: `add(n)` accumulates outstanding units,
`done()` retires one, and `wait()` parks until the count reaches
zero. The contract is `add` before `wait` and `done` paired with
`add`: an unmatched `done` releases `wait` early, and a missing
`done` parks it forever — the program ends under the exit policy
of §11.1. Unlike the handle classes, `WaitGroup` is ordinary
NLang code declared in the package source and built on `Mutex`
and `Channel`.

## 11.4 Channel

A `Channel<T>` passes values between contexts. The element type
argument is required: it drives the boxing on send and the
unboxing on receive, by the same mechanism as `List<T>` (§4.10);
reference elements pass through directly. A channel is a handle
class of `concurrent` (§11.2).

Construction takes two forms: `{}` creates an unbounded channel;
`Channel(n)` creates one with buffer capacity `n` — `0` means
unbounded, `n >= 1` bounds the buffer, and a negative `n` raises
`Exception` (§9.3).

- `send(v)` appends to the buffer. On an unbounded channel it
  never parks; on a bounded channel it parks in a queue while the
  buffer is full and resumes in arrival order as slots free.
- `recv()` takes the head value when the buffer is non-empty;
  otherwise it parks until a `send` or a `close`.
- `close()` closes the channel; it is idempotent, and values
  already in the buffer are kept.

Closing is observed through the catchable
`ChannelClosedException` (§9.3). After `close`, `recv` still
delivers the buffered values; once the buffer is drained, a
further `recv` raises the exception, and any `send` after the
close raises it at once. Parked waiters see the same boundary:
`close` first drains the buffer into the parked receivers, then
wakes the remaining receivers and every parked sender with the
exception.

## 11.5 Future and Promise

A `Future<T>` is the retrieval handle for a computation's return
value — produced by `thread.spawn` on a `func<T>` delegate
(§11.1), or by a `Promise`. It is a handle class of `thread`
(§11.2).

- `get()` parks until the target finishes, then yields its return
  value typed `T`. If the target failed, `get()` rethrows the
  original exception object (§9.3). Several contexts may `get()`
  one future; all wake with the same value or the same object.
  `get()` on a finished future returns immediately and may be
  called repeatedly. A context may not `get()` the future of its
  own spawn: that call is a *host-level* run-time error (§9.1),
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
  run-time errors (§9.1).

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

## 11.6 AtomicInt

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
belongs behind a `Mutex` (§11.2).

## 11.7 Permitted Interleavings

Under a single carrier the schedule is deterministic: a strict
first-in-first-out queue with a fixed back-edge budget, so the
same program with the same inputs produces the same interleaving
on every run, apart from `sleep` deadlines, which follow the
host's clock (§11.1).

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
