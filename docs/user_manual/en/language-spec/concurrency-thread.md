# The thread Package

The normative rules are in [specification §11.1](../specification/11-concurrency.md)
and [§17.1](../specification/17-concurrency-library.md).
The `thread` package is an ordinary standard-library package: its authoritative declarations live in `stdlib/thread.n` (completion and the symbol index treat it like any other package), while the implementations are built into the virtual machine itself. Calls need `import thread;` first (the examples in this chapter all import it), and a user package named `thread` collides with the standard library and is rejected by the duplicate-package rule. `spawn`'s typed contract — a `func<T>` delegate yields a `Future<T>` — is bound by the compiler on top of the declared `func<void>` base form.

| Member | Shape | Semantics |
|---|---|---|
| `thread.spawn(f)` | zero-argument `func<void>` delegate → `Thread`; zero-argument `func<T>` delegate → `Future<T>` | Spawns a new virtual thread and returns its handle object immediately; the delegate's return type decides the handle shape |
| `thread.join(t)` | `Thread` | Suspends until the target finishes; if the target failed, rethrows the original exception object into this context |
| `thread.yield()` | — | Voluntary handoff inside long compute loops: give other tasks a chance to run. Blocking calls (`sleep`, `recv`, `get`, `lock`, `acquire`) hand off automatically; a tight loop without `yield` is still preempted after about 1000 loop iterations — `yield` only hands off sooner |
| `thread.id()` | → `int` | The current context's identifier; main is always 0, spawned contexts are non-zero |
| `thread.sleep(ms)` | `int` milliseconds | Suspends until the deadline — the usual way to pace a polling loop or delay a retry; `sleep(0)` is equivalent to a yield; a negative duration is a runtime error (after compilation) |

## thread.spawn

The basic pattern is three steps: define a zero-argument function or
method, `thread.spawn` it to get a handle, then wait for the result
with `thread.join` (or `get()` on a `Future`). The shipped
`examples/conc_spawn_join.n` and `examples/conc_future.n` show exactly
this.

The argument must be a zero-argument delegate — a free-function reference, a bound-method reference, or a variable holding a delegate all work. A delegate with no return value (`func<void>`) yields a `Thread` handle; a delegate with a return value (`func<T>`) yields a `Future<T>` handle, and the return value comes back through `get()` (see [Future](concurrency-future.md)):

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
        return 1;              // join rethrows a failed context: unreachable
    } catch (Exception e) {
        if (e.message == "wboom") {
            return 30;         // the very instance thrown in the worker
        }
        return 1;
    }
}
```

`join` on an already-finished target returns immediately without waiting again; joining a context onto itself is a runtime error. A failed context's retained exception object is held by the scheduler until the run ends (context records are never pruned), so a late `join` still rethrows it.

## Probing a task's failure

Sometimes waiting is not the right move: a supervisor loop wants to check on a task without suspending. `thread.isFailed` and `thread.failure` are non-blocking reads of a spawned task's failure state:

| Member | Shape | Semantics |
|---|---|---|
| `thread.isFailed(t)` | `Thread` → `bool` | `true` once the target finished with an uncaught exception; `false` while it runs and after a clean finish. A pure read — it never marks the failure as observed |
| `thread.failure(t)` | `Thread` → `Exception` | The target's original exception object (the same instance `join` rethrows), or `null` when the target has not failed. The retrieval counts as observation, so a failure taken here no longer reports at run end |

Both functions validate the handle the same way `join` does (a null handle raises `NullPointerException`; a dead context id is a runtime error). They never suspend, so they are safe inside polling loops:

```nlang
import io;
import thread;

void fragile() {
    throw Exception("disk full");
}

int main() {
    Thread t = thread.spawn(fragile);
    int spins = 0;
    while (!thread.isFailed(t) && spins < 10000) {
        spins = spins + 1;
        thread.yield();
    }
    if (thread.isFailed(t)) {
        Exception e = thread.failure(t);
        io.print(e.message);       // disk full
    }
    return 0;
}
```

The two reads answer different questions and pair naturally: `isFailed` distinguishes "failed" from "still running or finished clean"; `failure` distinguishes "failed" from everything else by handing back the reason (null means not failed). Retrieving through `failure` marks the failure as observed — a task whose failure was taken this way is exempt from the run-end report that otherwise lists failures nobody ever waited for. `join` and `Future.get` keep marking the observation too; only `isFailed` leaves the state untouched, because peeking at the fact is not the same as taking the reason.
