# Future

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

`get()` is an honest blocking wait (the "no async coloring" point in [the concurrency model](concurrency.md#the-concurrency-model-blocking-style-no-async-coloring) made concrete): the caller parks and continues in place once the value is ready — this is NLang's `await`. Several contexts may `get()` the same `Future`; all of them wake with the same value. `get()` on an already-finished target returns immediately and may be called repeatedly. A failed context has no value to take — `get()` rethrows the original exception object (type, `message`, and `backtrace` fields preserved). Getting a `Future` spawned by yourself (handing the `Future` back to its spawner) is a runtime error naming the rule; nothing can ever wake that context, so the virtual machine catches it with the deadlock report (see [the main-exit policy and the deadlock report](concurrency-parking.md#main-exit-policy-and-the-deadlock-report)).
