# Promise

## When to use

A `Future`'s value comes from the return of a spawned function —
`thread.spawn` is the one source of retrieval handles. Sometimes the
value does not come from any spawned function at all: a host callback
needs to hand an external event into the world of waiting, one event
must wake several waiters at once, or a test needs exact control over
when a value becomes ready. `Promise<T>` is the manual completion
source: construct one, hand out its `Future`, and complete the value
yourself — from any context, at any time, exactly once.

```nlang
import io;
import thread;

class Waiter {
    public Promise<int> p;
    public void fire() { p.complete(42); }
}

int main() {
    Promise<int> p = Promise();
    Future<int> f = p.future();
    Waiter w = Waiter();
    w.p = p;
    Thread t = thread.spawn(w.fire);
    io.write(f.get().toString());
    thread.join(t);
    return 0;
}
```

## The handle

| Member | Semantics |
|---|---|
| `Promise()` | Constructs the completion source: a distributable, undecided `Future` exists immediately — no function needed up front |
| `future()` | Hands out a retrieval handle. Repeatable: each `Future` `get()`s independently, all sharing the one decision |
| `complete(v)` | Completes with value `v`: the first call wins, later calls are silently ineffective; every waiter wakes with `v` |
| `completeExceptionally(e)` | Completes with failure — exception object `e`: first-wins as well; a waiting `get()` rethrows `e` into its own context |

First-wins is the skeleton of the whole semantics: a value and a
failure are the same kind of completion — one decision, never
rewritten. Completion may come from any context: main completing
directly, a spawned task completing across contexts, or several
contexts racing to complete all work — the scheduler orders concurrent
`complete` calls and exactly one takes effect. The `Promise` itself is
an ordinary object with field semantics (NLang has no closures;
cross-context sharing goes through a class field or a container).

## Future.selectAny: multi-way waiting

The static call `Future.selectAny(futures)` takes an array of
`Future` values, suspends the caller until **any one** member decides,
and returns the winning member's index (the minimal index when
several are already decided):

```nlang
import io;

int main() {
    Promise<int> pa = Promise();
    Promise<int> pb = Promise();
    Future<int> a = pa.future();
    Future<int> b = pb.future();
    Future<int>[] arr = [a, b];
    int winner = Future.selectAny(arr);
    io.write(winner.toString());
    io.write(" ");
    io.write(a.get().toString());
    io.write("\n");
    pb.complete(2);
    pa.complete(1);
    return 0;
}
```

- **Failure decides too**: a failed target settles `selectAny` just
  the same; observing the failure is `get()`'s job — the winning
  index carries no throw, and `get()` on the winner rethrows its
  original exception object.
- **Losers keep running**: the other members run on untouched, their
  results retrievable by `get()` at any later point. A failure no one
  ever observes is still named by the run-end report (see
  [Suspension and scheduling limits](concurrency-suspension.md)).
- **Duplicates are legal**: one `Future` registered under several
  indices — the minimal index wins.
- **Edges**: an empty array, a `null` member, more than 64 members,
  or the current context's own `Future` in the wait set are runtime
  errors; self-waiting is the same rejection `get()` makes, message
  and all.

Before `selectAny`, "whichever is ready first" meant spawning one
waiter per source and converging through a channel — workable, but a
spawn and a channel round-trip per source. `selectAny` compresses the
same shape into one static call; a side-by-side example ships in the
installer's `examples/conc_*.n`.

## Known edges

- **Never completing is silent**: a `Promise` has no timeout; a source
  that never completes leaves its waiters parked until the deadlock
  report stops the run (see
  [Suspension and scheduling limits](concurrency-suspension.md#main-exit-policy-and-the-deadlock-report)).
  For a deadline, combine: spawn the wait and a `thread.sleep`
  separately and let whichever wakes first pick the path.
- **Multi-way waiting is `Future`-only**: there is no "receive from
  any" variant on channels. To receive from many channels, bridge each
  source through a `Promise` (one forwarding task per source,
  completing its own `Promise` with the received value) and
  `selectAny` over the `Future` array.
- **Value-returning `spawn` takes bare function references or bound
  delegate values**: `thread.spawn(obj.method)` as a typed spawn
  currently needs the explicit delegate local (`func<int> d =
  obj.method;`) or a free function; the void shape is unaffected.
