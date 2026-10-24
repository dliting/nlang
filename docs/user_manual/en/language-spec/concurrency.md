# Concurrency

NLang provides lightweight concurrency: the `thread` package spawns **virtual threads** — execution contexts that share a carrier pool and a global interpreter lock protecting shared storage, not operating-system threads of their own. `Mutex` and `Channel<T>` provide mutual exclusion and message-passing synchronization; `Future<T>` retrieves a spawned task's return value; `join` establishes completion order; the standard-library `concurrent` package adds `WaitGroup` and `Semaphore` on top. The `thread` package is part of the standard library — its declarations live in `stdlib/thread.n` — and like every standard-library package it needs `import thread;` before use. `Mutex`, `Channel<T>`, `Future<T>`, and the other handle types are built-in classes that need no import.

Scheduling determinism comes in tiers: under a single carrier (the library default) it is fully deterministic — a strict first-in-first-out (FIFO) rotation where a context yields only after exhausting a fixed back-edge quota (and only when another runnable context exists), so the same program with the same inputs produces the same interleaving; with multiple carriers, dequeuing applies wake affinity and the interleaving is unspecified (see [the GIL and multiple carriers](concurrency-gil-carriers.md)). `sleep` wakes in deadline order. Single-carrier tests may assert interleaving and final values; multi-carrier tests assert results after synchronization — never durations.

## The concurrency model: blocking style, no async coloring

NLang belongs to the **virtual-thread** family (like Go, and like Java's virtual threads): writing concurrent code reads like writing ordinary sequential code — when a wait happens, the context steps aside, and the scheduler interleaves the virtual threads between suspension points. Under this model, "single-threaded async" is the default shape: the library defaults to one carrier, and the tasks spawned by `thread.spawn` take turns on that single line of execution — waiting on timers, channels, and each other — solving the same problem as a JavaScript event loop, just written as blocking, straight-line code:

| Single-threaded async (JavaScript) | The NLang equivalent |
|---|---|
| event loop | the scheduler's run queue (one carrier by default) |
| `setTimeout(f, 100)` | `thread.spawn(f)` plus `thread.sleep(100)` |
| `await promise` / `Promise.all` | `f.get()` / `thread.join(t)` |
| callback nesting | plain sequential code |

This is why NLang has no `await` keyword and does not need one: functions are not async-colored and nothing compiles into a state machine; `f.get()` is an honest blocking wait — the current virtual thread parks, and once the value is ready it continues in place. To run on several carriers with real parallelism (pure-compute segments overlap directly; shared-store operations still serialize through the global interpreter lock), opt in with `SetCarrierThreadCount(n)` (see [the GIL and multiple carriers](concurrency-gil-carriers.md)).

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
| stop a group of waiters at once | `Channel.close()` broadcast |

## Where to look

- [The GIL and multiple carriers](concurrency-gil-carriers.md) — carriers, the global interpreter lock, blocking native windows, wake affinity, and what each determinism tier guarantees
- [The thread package](concurrency-thread.md) — `spawn`, `join`, `yield`, `id`, `sleep`
- [Future](concurrency-future.md) — retrieving a spawned task's return value
- [Mutex](concurrency-mutex.md) — non-reentrant mutual exclusion
- [Channel](concurrency-channel.md) — message passing and closing semantics
- [The concurrent package](concurrency-concurrent.md) — `WaitGroup` and `Semaphore`
- [Parking and scheduling limits](concurrency-parking.md) — exceptions across parking, the main-exit policy, the deadlock report, nested evaluation windows
