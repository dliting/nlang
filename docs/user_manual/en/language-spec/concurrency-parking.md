# Parking and Scheduling Limits

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
