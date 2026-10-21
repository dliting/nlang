# Concurrency

NLang provides lightweight concurrency: the `thread` package spawns **virtual threads** — execution contexts that share a carrier pool and run one at a time under a global interpreter lock, not operating-system threads of their own. `Mutex` and `Channel<T>` provide mutual exclusion and message-passing synchronization; `Future<T>` retrieves a spawned task's return value; `join` establishes completion order; the standard-library `concurrent` package adds `WaitGroup` and `Semaphore` on top. The `thread` package is part of the standard library — its declarations live in `stdlib/thread.n` — and like every standard-library package it needs `import thread;` before use. `Mutex`, `Channel<T>`, `Future<T>`, and the other handle types are built-in classes that need no import.

Scheduling is fully deterministic: carriers rotate contexts from a first-in-first-out (FIFO) queue, and a context yields only after exhausting a fixed back-edge quota (and only when another runnable context exists); `sleep` wakes in deadline order. The same program with the same inputs produces the same interleaving — tests assert order and final values, never durations.

## The concurrency model: blocking style, no async coloring

NLang belongs to the **virtual-thread** family (like Go, and like Java's virtual threads): writing concurrent code reads like writing ordinary sequential code — when a wait happens, the context steps aside, and the scheduler interleaves the virtual threads between suspension points. Under this model, "single-threaded async" is the default shape: the library defaults to one carrier, and the tasks spawned by `thread.spawn` take turns on that single line of execution — waiting on timers, channels, and each other — solving the same problem as a JavaScript event loop, just written as blocking, straight-line code:

| Single-threaded async (JavaScript) | The NLang equivalent |
|---|---|
| event loop | the scheduler's run queue (one carrier by default) |
| `setTimeout(f, 100)` | `thread.spawn(f)` plus `thread.sleep(100)` |
| `await promise` / `Promise.all` | `f.get()` / `thread.join(t)` |
| callback nesting | plain sequential code |

This is why NLang has no `await` keyword and does not need one: functions are not async-colored and nothing compiles into a state machine; `f.get()` is an honest blocking wait — the current virtual thread parks, and once the value is ready it continues in place. To run on several carriers with real parallelism (bounded by the global interpreter lock, native blocking windows excepted), opt in with `SetCarrierThreadCount(n)` (see [the GIL and multiple carriers](#the-gil-and-multiple-carriers)).

## The GIL and multiple carriers

The VM holds a **global interpreter lock** (GIL): exactly one virtual
thread executes bytecode at a time. Context switches happen only at
safepoints (function entry and loop back edges), bounded by a scheduler
budget of about 1000 back edges per turn. The GIL makes bytecode
execution mutually exclusive, so language code never has to guard
shared memory against a second virtual thread.

A **carrier** is an operating system thread that runs virtual threads.
The library defaults to a single carrier; embedding hosts opt in with
`VmExecutor::SetCarrierThreadCount`, and `nvm` defaults to at most four
(`--carriers=N`, 1..64). Multi-carrier parallelism comes from one kind
of window only: a native call that declared a blocking section under
version 5 of the native application binary interface (ABI) — the VM
releases the GIL for its duration, so other carriers run other virtual
threads (typically overlapping an input/output wait with other work),
and the GIL is reacquired before the native returns. A third-party
native that does not declare its blocking section holds the GIL for the
whole call and every carrier stalls.

Determinism: the same program with the same input produces the same
interleaving (FIFO queue plus a fixed back-edge budget), and changing
the carrier count does not change the observable behavior of a program
that never blocks natively — tests assert order and final values, never
timing. `sleep` is a scheduling hint with a lower bound: once every
carrier is parked, a sleeper wakes within the operating system's timer
granularity (about 15.6 ms on Windows). A paused debugger holds the GIL
and freezes every virtual thread (see
[Known limitations](known-limitations.md)).

## The thread package

The `thread` package is an ordinary standard-library package: its authoritative declarations live in `stdlib/thread.n` (completion and the symbol index treat it like any other package), while the implementations are built into the virtual machine itself. Calls need `import thread;` first (the examples on this page all import it), and a user package named `thread` collides with the standard library and is rejected by the duplicate-package rule. `spawn`'s typed contract — a `func<T>` delegate yields a `Future<T>` — is bound by the compiler on top of the declared `func<void>` base form.

| Member | Shape | Semantics |
|---|---|---|
| `thread.spawn(f)` | zero-argument `func<void>` delegate → `Thread`; zero-argument `func<T>` delegate → `Future<T>` | Spawns a new virtual thread and returns its handle object immediately; the delegate's return type decides the handle shape |
| `thread.join(t)` | `Thread` | Suspends until the target finishes; if the target failed, rethrows the original exception object into this context |
| `thread.yield()` | — | Voluntary yield; the context goes to the back of the run queue |
| `thread.id()` | → `int` | The current context's identifier; main is always 0, spawned contexts are non-zero |
| `thread.sleep(ms)` | `int` milliseconds | Suspends until the deadline; `sleep(0)` is equivalent to a yield; a negative duration is a runtime error (after compilation) |

### thread.spawn

The argument must be a zero-argument delegate — a free-function reference, a bound-method reference, or a variable holding a delegate all work. A delegate with no return value (`func<void>`) yields a `Thread` handle; a delegate with a return value (`func<T>`) yields a `Future<T>` handle, and the return value comes back through `get()` (next section):

```nlang
import io;
import thread;

class Worker {
    public int tag;
    public void run() {
        io.write(tag.toString());
    }
}

int main() {
    Worker w = Worker();
    w.tag = 7;
    Thread t = thread.spawn(w.run);
    thread.join(t);
    return 0;
}
```

The spawned context shares the same heap as its spawner: fields, containers, and channels are directly visible on both sides. The compiler rejects non-delegate arguments and delegates with parameters; delegates pointing at `native` or builtin functions are rejected at runtime.

## Future

Spawning a delegate with a return value (`func<T>`) yields a `Future<T>` — the retrieval handle for the return value:

| Member | Semantics |
|---|---|
| `get()` | Blocking take: parks while the target runs, then delivers the return value as the declared type `T`; if the target failed, rethrows the original exception object into this context (the `join` shape) |
| `isDone()` | Non-parking probe: `true` once the target has finished (normally or by failure), `false` otherwise |

```nlang
import io;
import thread;

int produce() {
    thread.sleep(10);
    return 42;
}

int main() {
    Future<int> f = thread.spawn(produce);
    io.write(f.get().toString());
    return 0;
}
```

`get()` is an honest blocking wait (the "no async coloring" point in the model section above made concrete): the caller parks and continues in place once the value is ready — this is NLang's `await`. Several contexts may `get()` the same `Future`; all of them wake with the same value. `get()` on an already-finished target returns immediately and may be called repeatedly. A failed context has no value to take — `get()` rethrows the original exception object (type, `message`, and `backtrace` fields preserved). Getting a `Future` spawned by yourself (handing the `Future` back to its spawner) is a runtime error naming the rule; nothing can ever wake that context, so the virtual machine catches it with the deadlock report (see [main-exit policy and the deadlock report](#main-exit-policy-and-the-deadlock-report)).

## thread.join

`join` suspends the calling context until the target finishes. Several contexts may `join` the same target; all of them are woken. If the target **failed** (exited on an uncaught exception), `join` rethrows the **original exception object** into each waiter — type, `message`, and `backtrace` fields are preserved, so what you `catch` is the very instance thrown inside the spawned context:

```nlang
import io;
import thread;

void worker() {
    throw Exception("wboom");
}

int main() {
    Thread t = thread.spawn(worker);
    try {
        thread.join(t);
        return 4;
    } catch (Exception e) {
        if (e.message == "wboom") {
            return 0;
        }
        return 2;
    }
}
```

`join` on an already-finished target returns immediately without waiting again; joining a context onto itself is a runtime error. A failed context's retained exception object is held by the scheduler until the run ends (context records are never pruned), so a late `join` still rethrows it.

## Mutex

`Mutex` is a non-reentrant mutual-exclusion lock: locking it again while holding it parks forever (see the deadlock report below).

| Member | Semantics |
|---|---|
| `lock()` | Acquires the lock; parks in a queue while it is held, woken in arrival (first-in-first-out) order |
| `unlock()` | Releases; unlocking a lock you do not hold is a runtime error naming the rule |
| `tryLock()` | Non-parking probe: returns `true` and acquires on success, `false` if held |

`yield` and `sleep` inside a critical section still yield — the mutual-exclusion guarantee does not lapse because of yielding:

```nlang
import io;
import thread;

class Counter {
    public Mutex gate;
    public int n;
    public void bump() {
        gate.lock();
        n = n + 1;
        thread.yield();
        gate.unlock();
    }
}

int main() {
    Counter c = Counter();
    c.gate = Mutex();
    Thread a = thread.spawn(c.bump);
    Thread b = thread.spawn(c.bump);
    Thread d = thread.spawn(c.bump);
    thread.join(a);
    thread.join(b);
    thread.join(d);
    io.write(c.n.toString());
    return 0;
}
```

Each `bump` yields once; without the mutex the interleaving necessarily loses updates. With it, the output is always `3`.

## Channel

`Channel<T>` is a message channel. The element type argument is not optional (a bare `Channel` is a compile error — the element type drives value boxing and receiver-side unboxing). Two construction forms:

```nlang
Channel<int> a = {};         // unbounded: no buffer limit, send never parks
Channel<int> b = Channel(2); // bounded: capacity 2, a full buffer parks send
```

The capacity constructor takes exactly one `int`: `0` is equivalent to unbounded, `n >= 1` is a bounded capacity, and a negative value is a runtime error (the message states the accepted range); the empty initializer `{}` is the unbounded default. On a bounded channel, `send` parks in a first-in-first-out queue when the buffer is full; a `recv` frees a slot and wakes the head sender — earlier sends enter the buffer first, and order is preserved.

| Member | Semantics |
|---|---|
| `send(v)` | Never parks on an unbounded channel; on a bounded channel parks in queue when the buffer is full, resuming in arrival order as slots free |
| `recv()` | Takes the head when the buffer is non-empty; otherwise parks until a `send` or `close` |
| `close()` | Closes the channel; idempotent, values already in the buffer are not lost |

Closing semantics are drawn by the catchable `ChannelClosedException`: after `close`, values already in the buffer are still delivered by `recv` (sent values outrank the closing signal); once drained, a further `recv` — and any `send` after closing — raises `ChannelClosedException` (a subclass of `Exception`, catchable for recovery). Parked waiters see the same boundary: `close` first drains the buffer into them, then wakes the remaining receivers and every parked sender with that exception. When you use "closed" as a broadcast signal, the receiving side finishes with `try`/`catch`:

```nlang
import io;
import thread;

class Reader {
    public Channel<int> ch;
    public int total;
    public void drain() {
        bool open = true;
        while (open) {
            try {
                total = total + ch.recv();
            } catch (ChannelClosedException e) {
                open = false;
            }
        }
    }
}

int main() {
    Reader r = Reader();
    r.ch = Channel(4);
    Thread t = thread.spawn(r.drain);
    r.ch.send(10);
    r.ch.send(20);
    r.ch.close();
    thread.join(t);
    io.write(r.total.toString());
    return 0;
}
```

Primitive elements are boxed on send and unboxed on receive (the same mechanism as `List<T>`); reference elements pass through directly. The value `0` is a legitimate boxed value, constructively distinguishable from "no value" — the closing boundary is expressed by the exception, not by spending a value.

When the sender arrives first, `recv` takes straight from the buffer; when the receiver arrives first it parks, and the `send` appends and wakes it:

```nlang
import io;
import thread;

class Peer {
    public Channel<string> ch;
    public void shout() {
        ch.send("a");
        io.write("w");
    }
}

int main() {
    Peer p = Peer();
    p.ch = {};
    Thread t = thread.spawn(p.shout);
    io.write(p.ch.recv());
    thread.join(t);
    return 0;
}
```

Main parks on `recv` first; the worker's `send` delivers the value and wakes main, but the worker itself does not park — it writes `w` immediately, then main resumes and writes the value it received. The output is `wa`: the value has flowed from sender to receiver.

## Standard-library synchronization tools

On top of the kernel primitives, the `concurrent` standard-library package provides two common synchronization tools (available after `import concurrent;`):

| Class | Shape | Semantics |
|---|---|---|
| `WaitGroup` | `add(n)` / `done()` / `wait()` | A counting barrier (Go-shaped): `add` accumulates outstanding units, each unit calls `done` once, and `wait` parks until the count reaches zero. Contract: `add` before `wait`, and `done` paired with `add` — an unmatched `done` releases `wait` early, a missing `done` parks it until the deadlock report |
| `Semaphore` | `Semaphore(n)` / `acquire()` / `release()` | A counting semaphore: `acquire` takes a permit (yielding while the count is zero), `release` returns one |

```nlang
import concurrent;
import io;
import thread;

class Job {
    public WaitGroup wg;
    public Mutex gate;
    public int total;
    public void work() {
        gate.lock();
        total = total + 10;
        gate.unlock();
        wg.done();
    }
}

int main() {
    Job j = Job();
    j.gate = Mutex();
    j.wg = WaitGroup();
    j.wg.add(4);
    Thread a = thread.spawn(j.work);
    Thread b = thread.spawn(j.work);
    Thread c = thread.spawn(j.work);
    Thread d = thread.spawn(j.work);
    j.wg.wait();
    io.write(j.total.toString());
    thread.join(a);
    thread.join(b);
    thread.join(c);
    thread.join(d);
    return 0;
}
```

`wait` is a real barrier — reading `40` holds exactly when all four increments landed before the read. `Semaphore` currently waits by yielding (the kernel has no condition-variable primitive yet): an `acquire` against a zero count takes turns via `thread.yield` instead of parking, without monopolizing a carrier.

## Exceptions and parking are orthogonal

Parking (`sleep`, `join`, contending `lock`, empty-channel `recv`, preemption) preserves the context's full execution state: frames, local variables, `try`/`catch`/`finally` handler state, and the instruction position are all frozen as-is, and execution continues after resumption. Therefore:

- Parking inside a `try` body followed by a `throw` still lands in that `try`'s `catch` (across frames too);
- `finally` still runs — exactly once — after a `try` body that parked;
- Parking inside an entered `catch` handler still leaves `throw;` (rethrow) working after resumption.

An **uncaught** exception in a spawned context propagates to neither the host nor main: the context is marked failed, its frames are dropped, and the failure surfaces only through `join`, as the original exception object.

## Main-exit policy and the deadlock report

When `main` returns, the program ends: contexts still parked or queued are dropped silently (there is no implicit wait-for-all — one sleeping worker should not hang the whole program; see the [known limitations](known-limitations.md)).

When `main` is parked and no runnable context remains (for example, an empty-channel `recv` whose sender will never come, or a mutex relocked by its own holder), the virtual machine reports the deadlock as a runtime error — the message contains `deadlock` — instead of hanging the process; tools and embedding hosts can catch that error like any other.

## Restrictions inside nested evaluation windows

Nested windows such as `toString` evaluation cannot suspend: `thread.yield`, `thread.sleep`, `thread.join` on an unfinished target, `Future.get` on an unfinished target, a contended `lock`, empty-channel `recv`, and `send` on a full bounded channel are all rejected as runtime errors (the message contains `cannot suspend`), as is `thread.spawn`. An embedding host's `call()` window is the same shape — see [Embedding NLang](../libraries/embedding-api.md).
