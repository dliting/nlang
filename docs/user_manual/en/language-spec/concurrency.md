# Concurrency

NLang provides lightweight concurrency: the `thread` package spawns **virtual threads** — execution contexts that time-share a small pool of shared **carriers** (operating system threads that run virtual threads); a virtual thread is not an operating system thread of its own, and storage protection is covered in [Carriers and Parallelism](concurrency-carriers.md). `Mutex` and `Channel<T>` provide mutual exclusion and message-passing synchronization; `Semaphore` caps how many tasks run a section at once; `Future<T>` retrieves a spawned task's return value; `join` establishes completion order; the standard-library `concurrent` package adds `WaitGroup` on top. The `thread` package is part of the standard library — its declarations live in `stdlib/thread.n` — and like every standard-library package it needs `import thread;` before use. `Mutex`, `Channel<T>`, `Semaphore`, `Future<T>`, and the other handle types are built-in classes that need no import.

Scheduling determinism: under a single carrier (the library default) it is fully deterministic — a strict first-in-first-out (FIFO) rotation where each virtual thread runs about 1000 loop iterations before yielding (and only when another runnable context exists), so the same program with the same inputs produces the same interleaving; with multiple carriers the scheduling order is unspecified (see [Carriers and Parallelism](concurrency-carriers.md)). `sleep` wakes in deadline order. Single-carrier tests may assert interleaving and final values; multi-carrier tests assert only results after synchronization — never durations.

## The concurrency model: blocking style, no async coloring

NLang belongs to the **virtual-thread** family (like Go, and like Java's virtual threads): writing concurrent code reads like writing ordinary sequential code — when a wait happens, the context steps aside, and the scheduler interleaves the virtual threads between suspension points. Under this model, "single-threaded async" is the default shape: the library defaults to one carrier, and the tasks spawned by `thread.spawn` take turns on that single line of execution — waiting on timers, channels, and each other — solving the same problem as a JavaScript event loop, just written as blocking, straight-line code:

| Single-threaded async (JavaScript) | The NLang equivalent |
|---|---|
| event loop | the scheduler's run queue (one carrier by default) |
| `setTimeout(f, 100)` | `thread.spawn(f)` plus `thread.sleep(100)` |
| `await promise` / `Promise.all` | `f.get()` / `thread.join(t)` |
| callback nesting | plain sequential code |

This is why NLang has no `await` keyword and does not need one: functions are not async-colored and nothing compiles into a state machine; `f.get()` is an honest blocking wait — the current virtual thread suspends, and once the value is ready it continues in place. To run on several carriers with real parallelism (pure-compute segments and object field access overlap directly; containers, strings, streams, and synchronization primitives each take their own lock, one per data category), opt in with `SetCarrierThreadCount(n)` (see [Carriers and Parallelism](concurrency-carriers.md)).

## Choosing the right tool

Start from the task you have, not from the tool list. The shipped
`examples/conc_*.n` walk through each row:

| You want to… | Use |
|---|---|
| start a background task | `thread.spawn(f)` |
| get a task's return value | spawn a returning function → `Future<T>` → `get()` |
| hand values between tasks | `Channel<T>` (send/recv/close) |
| guard shared mutable state | `Mutex` (lock/unlock/tryLock) |
| cap how many tasks run a section at once | `Semaphore(n)` |
| wait for a batch of tasks | `WaitGroup` (add/done/wait) or `join` each |
| wake a group of waiters at once | `Channel.close()` broadcast |

## Where to look

- [Carriers and Parallelism](concurrency-carriers.md) — the execution model, storage protection, which code runs truly in parallel, determinism, and choosing the carrier count
- [The thread package](concurrency-thread.md) — `spawn`, `join`, `yield`, `id`, `sleep`
- [Future](concurrency-future.md) — retrieving a spawned task's return value
- [Mutex](concurrency-mutex.md) — non-reentrant mutual exclusion
- [Semaphore](concurrency-semaphore.md) — a counting semaphore
- [Channel](concurrency-channel.md) — message passing and closing semantics
- [The concurrent package](concurrency-concurrent.md) — `WaitGroup`
- [Suspension and scheduling limits](concurrency-suspension.md) — suspension versus blocking, exceptions across suspension, the main-exit policy, the deadlock report, restrictions inside re-entrant evaluation
