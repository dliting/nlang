# Concurrency

NLang provides lightweight concurrency: the `thread` namespace spawns **virtual threads** — execution contexts interleaved on a single carrier, not operating-system threads. `Mutex` and `Channel<T>` provide mutual exclusion and message-passing synchronization; `join` establishes completion order. All primitives work without any `import` (`thread` is a reserved package name — declaring your own package with that name is rejected at compile time with a message telling you to rename it).

Scheduling is fully deterministic: a single carrier rotates contexts from a first-in-first-out (FIFO) queue, and a context yields only after exhausting a fixed back-edge quota (and only when another runnable context exists); `sleep` wakes in deadline order. The same program with the same inputs produces the same interleaving — tests assert order and final values, never durations.

## The thread namespace

| Member | Shape | Semantics |
|---|---|---|
| `thread.spawn(f)` | zero-argument `func<void>` delegate → `Thread` | Spawns a new virtual thread; returns its `Thread` object immediately |
| `thread.join(t)` | `Thread` | Suspends until the target finishes; if the target failed, rethrows the original exception object into this context |
| `thread.yield()` | — | Voluntary yield; the context goes to the back of the run queue |
| `thread.id()` | → `int` | The current context's identifier; main is always 0, spawned contexts are non-zero |
| `thread.sleep(ms)` | `int` milliseconds | Suspends until the deadline; `sleep(0)` is equivalent to a yield; a negative duration is a runtime error (after compilation) |

### thread.spawn

The argument must be a zero-argument `func<void>` delegate — a free-function reference, a bound-method reference, or a variable holding a delegate all work:

```nlang
import io;

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

The spawned context shares the same heap as its spawner: fields, containers, and channels are directly visible on both sides. The compiler rejects non-delegate arguments, delegates with parameters, and delegates with a return value (`func<void>` is the only shape); delegates pointing at `native` or builtin functions are rejected at runtime.

### thread.join

`join` suspends the calling context until the target finishes. Several contexts may `join` the same target; all of them are woken. If the target **failed** (exited on an uncaught exception), `join` rethrows the **original exception object** into each waiter — type, `message`, and `backtrace` fields are preserved, so what you `catch` is the very instance thrown inside the spawned context:

```nlang
import io;

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

`Channel<T>` is an unbounded message channel. The element type argument is not optional (a bare `Channel` is a compile error — the element type drives value boxing and receiver-side unboxing). The only construction form is the empty initializer:

```nlang
Channel<int> ci = {};
Channel<string> cs = {};
```

| Member | Semantics |
|---|---|
| `send(v)` | Appends to the buffer, **never parks**; a runtime error if the channel is closed |
| `recv()` | Takes the head when the buffer is non-empty; otherwise parks until a `send` or `close` |
| `close()` | Closes the channel; idempotent, values already in the buffer are not lost |

Closing semantics: `recv` drains buffered values first; once the buffer is empty and the channel is closed, it returns `null`. When the element type is a reference type (class, interface, and so on), the result compares directly against `null`; primitive element types (such as `Channel<int>`) deliver through unboxing, which cannot represent `null` — a `recv` on a closed, drained integer channel surfaces as a runtime error. Use a reference element type if you need sentinel-value semantics.

Primitive elements are boxed on send and unboxed on receive (the same mechanism as `List<T>`); reference elements pass through directly. The value `0` is a legitimate boxed value, constructively distinguishable from the `null` sentinel that means "no value".

When the sender arrives first, `recv` takes straight from the buffer; when the receiver arrives first it parks, and the `send` appends and wakes it:

```nlang
import io;

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

Nested windows such as `toString` evaluation cannot suspend: `thread.yield`, `thread.sleep`, `thread.join` on an unfinished target, a contended `lock`, and empty-channel `recv` are all rejected as runtime errors (the message contains `cannot suspend`), as is `thread.spawn`. An embedding host's `call()` window is the same shape — see [Embedding NLang](../libraries/embedding-api.md).
