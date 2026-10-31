# Mutex

## When to use

Use a `Mutex` for shared mutable state — a counter, a cache, a flag —
that several tasks read and write: the lock makes each read-modify-write
sequence atomic, so no update is lost. A single hot value (a pure
counter, a flag) can go lighter — [AtomicInt](concurrency-atomic.md)
makes the read-modify-write one step, no mutex needed. `tryLock()` fits the
attempt-without-waiting shape: take the lock when it is free, otherwise
take another route (skip this round, fall back) instead of suspension in
the queue.

One rule of thumb splits the three synchronization styles: state you
share → `Mutex`; data you hand over → [Channel](concurrency-channel.md);
admission you cap → [Semaphore](concurrency-semaphore.md).

## The lock

`Mutex` is a non-reentrant mutual-exclusion lock: locking it again while holding it suspends forever (see [the deadlock report](concurrency-suspension.md#main-exit-policy-and-the-deadlock-report)) — composed or recursive methods take [ReentrantMutex](concurrency-reentrant-mutex.md).

| Member | Semantics |
|---|---|
| `lock()` | Acquires the lock; suspends in a queue while it is held, woken in arrival (first-in-first-out) order |
| `unlock()` | Releases; unlocking a lock you do not hold is a runtime error naming the rule |
| `tryLock()` | Non-suspension probe: returns `true` and acquires on success, `false` if held |

`yield` and `sleep` inside a critical section still yield — the mutual-exclusion guarantee does not lapse because of yielding:

```nlang
import io;
import thread;
import concurrent;

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
