# Parking and Scheduling Limits

## Exceptions and parking are orthogonal

Parking (`sleep`, `join`, contending `lock`, empty-channel `recv`, preemption) preserves the context's full execution state: frames, local variables, `try`/`catch`/`finally` handler state, and the instruction position are all frozen as-is, and execution continues after resumption. Therefore:

- Parking inside a `try` body followed by a `throw` still lands in that `try`'s `catch` (across frames too);
- `finally` still runs — exactly once — after a `try` body that parked;
- Parking inside an entered `catch` handler still leaves `throw;` (rethrow) working after resumption.

An **uncaught** exception in a spawned context propagates to neither the host nor main: the context is marked failed and its frames are dropped. The failure surfaces only on the waiting side — `join` and `Future.get` rethrow the task's original exception object — so put the `try`/`catch` where you wait, not where you spawn.

## Main-exit policy and the deadlock report

Your program ends when `main` returns — spawned tasks do not keep it alive; wait for what you need (`join`, `get`, `wait`) before returning. Contexts still parked or queued are dropped silently: there is no implicit wait-for-all, because one sleeping worker should not hang the whole program (see the [known limitations](known-limitations.md)).

When `main` is parked and no runnable context remains (for example, an empty-channel `recv` whose sender will never come, or a mutex relocked by its own holder), the virtual machine reports the deadlock as a runtime error — the message contains `deadlock` — instead of hanging the process; tools and embedding hosts can catch that error like any other.

The message names the condition, not the culprit: it does not list which context waits on what. To find the wait that has no wake path, stop the program under ndb before the last wait forms — a breakpoint or a step near the spawn or wait site — and run `threads`: each row shows a context's state (`parked-chan-recv`, `parked-join`, `parked-mutex`, …), so the parked rows tell you who is waiting for whom. (The deadlock error itself ends the debug session, so inspect at a stop, not after the report.) The fix is almost always on the wake side: a `send` or `close` for every `recv`, an `unlock` for every `lock`, a `done` for every outstanding `add`.

## Restrictions inside nested evaluation windows

Nested windows such as `toString` evaluation cannot suspend: `thread.yield`, `thread.sleep`, `thread.join` on an unfinished target, `Future.get` on an unfinished target, a contended `lock`, empty-channel `recv`, and `send` on a full bounded channel are all rejected as runtime errors (the message contains `cannot suspend`), as is `thread.spawn`. An embedding host's `call()` window is the same shape — see [Embedding NLang](../libraries/embedding-api.md).
