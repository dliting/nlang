# Mutex

`Mutex` is a non-reentrant mutual-exclusion lock: locking it again while holding it parks forever (see [the deadlock report](concurrency-parking.md#main-exit-policy-and-the-deadlock-report)).

| Member | Semantics |
|---|---|
| `lock()` | Acquires the lock; parks in a queue while it is held, woken in arrival (first-in-first-out) order |
| `unlock()` | Releases; unlocking a lock you do not hold is a runtime error naming the rule |
| `tryLock()` | Non-parking probe: returns `true` and acquires on success, `false` if held |

`yield` and `sleep` inside a critical section still yield — the mutual-exclusion guarantee does not lapse because of yielding:

```nlang
import io;
import thread;

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
