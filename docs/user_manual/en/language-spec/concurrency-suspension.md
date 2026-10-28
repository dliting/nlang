# Suspension and Scheduling Limits

## Suspension versus blocking

A virtual thread **suspends** at a wait point: its full execution state — the call stack, local variables, `try`/`catch`/`finally` handler state, and the instruction position — freezes, it yields its carrier, and it continues in place after being woken. What suspends is the virtual thread, while the carrier goes on executing other virtual threads; **blocking** means the carrier thread itself waits inside an operating system call. Everywhere below, "suspends" means the former.

## Exceptions and suspension are orthogonal

Suspension (`sleep`, `join`, contending `lock`, zero-permit `acquire`, empty-channel `recv`, preemption) preserves the context's full execution state, and execution continues after resumption. Therefore:

- Suspending inside a `try` body followed by a `throw` still lands in that `try`'s `catch` (across function calls too);
- `finally` still runs — exactly once — after a `try` body that suspended;
- Suspending inside an entered `catch` handler still leaves `throw;` (rethrow) working after resumption.

An **uncaught** exception in a spawned context propagates to neither the host nor main: the context is marked failed and its call stack is dropped. The failure surfaces only on the waiting side — `join` and `Future.get` rethrow the task's original exception object — so put the `try`/`catch` where you wait, not where you spawn. A failure nobody ever waited for is not silently lost: at run end the VM prints one stderr warning per such task (task id, exception type and message).

## Main-exit policy and the deadlock report

Your program ends when `main` returns — spawned tasks do not keep it alive; wait for what you need (`join`, `get`, `wait`) before returning. Contexts still suspended or queued are dropped silently: there is no implicit wait-for-all, because one sleeping worker should not hang the whole program (see the [known limitations](known-limitations.md)).

When `main` is suspended and no runnable context remains (for example, an empty-channel `recv` whose sender will never come, or a mutex relocked by its own holder), the virtual machine reports the deadlock as a runtime error — the message contains `deadlock` — instead of hanging the process; tools and embedding hosts can catch that error like any other.

The message names the condition, not the culprit: it does not list which context waits on what. To find the wait that has no wake path, stop the program under ndb before the last wait forms — a breakpoint or a step near the spawn or wait site — and run `threads`: each row shows a context's state (`parked-chan-recv`, `parked-join`, `parked-mutex`, `parked-sem`, … — `parked` means suspended), so the suspended rows tell you who is waiting for whom. (The deadlock error itself ends the debug session, so inspect at a stop, not after the report.) The fix is almost always on the wake side: a `send` or `close` for every `recv`, an `unlock` for every `lock`, a `release` for every `acquire`, a `done` for every outstanding `add`.

## Suspension inside re-entrant evaluation

The virtual machine sometimes re-enters your code in the middle of an operation — calling your `toString` override while formatting a collection for output, or executing a script function for a host callback. Inside such **re-entrant evaluation** the wait points behave exactly like anywhere else: `thread.yield`, `thread.sleep`, `thread.join` on an unfinished target, `Future.get` on an unfinished target, a contended `lock`, a zero-permit `acquire`, empty-channel `recv`, and `send` on a full bounded channel all suspend normally. The window freezes in place, other contexts run on the same carrier while it waits, and the `toString` override (or callback) resumes and returns its value when its wait completes — the formatting operation it was serving continues as if nothing happened. `thread.spawn` inside such a window is likewise fine: the new task becomes schedulable at once.

One shape still rejects suspension: an embedding host's `call()` window **outside a run**. With no scheduler carrier in flight, nothing could run other contexts while the window waits, so the wait points — and `thread.spawn`, which would never be scheduled — fail fast with a catchable runtime error (the message contains `cannot suspend`); see [Embedding NLang](../libraries/embedding-api.md). Host callbacks that fire **during** a run suspend normally, like any other wait. The implicit `toString` call a string-concatenation expression (`"x" + obj`) makes on a class operand compiles to an ordinary method call and was never re-entrant evaluation at all.

A deadlock reached from inside a window is reported like any other deadlock (the message contains `deadlock`) — for example a `toString` override whose `join` target is itself stuck waiting on the joiner.
