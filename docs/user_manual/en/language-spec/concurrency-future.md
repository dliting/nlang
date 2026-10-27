# Future

## When to use

Reach for a `Future` when a task produces a value you need in order to
keep computing: spawn the work, carry on with everything that does not
depend on it, and call `get()` at the point the value is actually
required — the wait lands exactly where the dependency is. Several
contexts can wait on the same `Future`; all wake with the same value.
If you only need to know that a task finished, `thread.join` is the
lighter handle.

## The handle

Spawning a delegate with a return value (`func<T>`) yields a `Future<T>` — the retrieval handle for the return value:

| Member | Semantics |
|---|---|
| `get()` | Blocking take: suspends while the target runs, then delivers the return value as the declared type `T`; if the target failed, rethrows the original exception object into this context (the `join` shape) |
| `isDone()` | Non-suspension probe: `true` once the target has finished (normally or by failure), `false` otherwise |

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

`get()` is an honest blocking wait (the "no async coloring" point in [the concurrency model](concurrency.md#the-concurrency-model-blocking-style-no-async-coloring) made concrete): the caller suspends and continues in place once the value is ready — this is NLang's `await`. Several contexts may `get()` the same `Future`; all of them wake with the same value. `get()` on an already-finished target returns immediately and may be called repeatedly. A failed context has no value to take — `get()` rethrows the original exception object (type, `message`, and `backtrace` fields preserved). Getting a `Future` spawned by yourself (handing the `Future` back to its spawner) is a runtime error naming the rule; nothing can ever wake that context, so the virtual machine catches it with the deadlock report (see [the main-exit policy and the deadlock report](concurrency-suspension.md#main-exit-policy-and-the-deadlock-report)).

When the value does not come from a spawned function, construct and
complete the `Future` by hand with a [Promise](concurrency-promise.md);
for "whichever of several `Future` values decides first", use
`Future.selectAny` (also covered in [Promise](concurrency-promise.md#futureselectany-multi-way-waiting)).
