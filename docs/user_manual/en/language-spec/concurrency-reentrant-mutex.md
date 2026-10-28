# ReentrantMutex

## When to use

`ReentrantMutex` is the reentrant counterpart of `Mutex`: the owning
context may `lock` again while holding it, without parking. It removes
the easiest `Mutex` trap — **method composition**. A public method that
holds the lock and then calls another locking method of the same class
self-deadlocks a `Mutex` (suspends until the deadlock report); method
composition is the most ordinary shape there is, and recursion is the
same story.

Choosing: default to `Mutex` — non-reentrancy lets the deadlock report
expose "locked but never released" design mistakes. Use
`ReentrantMutex` when the locked methods compose or recurse (outer and
inner both lock the same one). "Forgot to release" on a
`ReentrantMutex` is still caught by the deadlock report — reentrancy
exempts only same-context re-acquisition, never a missing release.

## The lock

| Member | Semantics |
|---|---|
| `lock()` | Free → take it; **self-held → hold count +1, returns at once** (never parks); held by another → parks in FIFO arrival order |
| `unlock()` | Hold count −1, **release or handoff only at zero**; unlocking without holding is a runtime error naming the rule |
| `tryLock()` | Non-parking probe: free or self-held → `true` (self-held also counts +1); held by another → `false` |

Handoff matches `Mutex`: at zero the ownership moves to the front
waiter, and the new owner starts from a fresh count of 1 — a hold count
never crosses contexts. A contended `lock()` parks through the same
handshake as `Mutex` and suspends normally inside re-entrant
evaluation (a carrier-less host `call()` window is the one shape that
still rejects it, `cannot suspend`); `tryLock()` never parks at all.

## Composed methods

```nlang
import io;
import thread;

class Service {
    public ReentrantMutex gate;
    public int count;

    public void outer() {
        gate.lock();
        this.inner();      //inner locks the SAME mutex — count +1, no park
        count = count + 1;
        gate.unlock();
    }

    public void inner() {
        gate.lock();
        count = count + 10;
        gate.unlock();
    }
}

int main() {
    Service s = Service();
    s.gate = ReentrantMutex();
    Thread t = thread.spawn(s.outer);
    thread.join(t);
    io.print(s.count);     //11: the two lock/unlock pairs balance to zero
    return 0;
}
```

`outer` holds the lock and `inner` locks again — on a `Mutex` that is
exactly the self-deadlock; on a `ReentrantMutex` the hold count reaches
2 and two `unlock`s bring it to zero, where the lock becomes visible to
other contexts again.

## Recursion

A recursive function that locks on entry and unlocks on return is the
same story one level per frame — a depth-8 recursion lands at a hold
count of exactly zero, with the lock's guarantees unchanged by depth.
This matches the hold-count model of Python's `threading.RLock` and
Java's `ReentrantLock`.

## Division of labor with Mutex

| Shape | Use |
|---|---|
| One critical section, no composed methods | [Mutex](concurrency-mutex.md) |
| A public method and its callees both lock | `ReentrantMutex` |
| Locking inside a recursive body | `ReentrantMutex` |
| Admission limiting | [Semaphore](concurrency-semaphore.md) |

The two coexist: each guards its own sections, chosen per member in the
same program.
