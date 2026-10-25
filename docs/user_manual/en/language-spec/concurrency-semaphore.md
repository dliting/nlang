# Semaphore

## When to use

To cap how many tasks run a section at once — resource pools,
connection limits, at most N concurrent computations — use
`Semaphore(n)`: `acquire()` a permit before entering, `release()` it on
the way out, and keeping the pair matched keeps the count honest. The
difference from `Mutex`: a mutex admits exactly one task at a time, a
semaphore admits at most N. One rule of thumb separates three routes:
shared state goes through a [mutex](concurrency-mutex.md), handed-off
data goes through a [channel](concurrency-channel.md), and admission
counting goes through a semaphore.

## Permits

`Semaphore` is a built-in handle class and needs no import. The
constructor `Semaphore(n)` sets the initial permit count `n`: a
negative value is a runtime error, and `0` is legal — the
signal-switch shape, where waiters suspend on a zero-permit semaphore
until some future event calls `release`.

| Member | Semantics |
|---|---|
| `acquire()` | Takes one permit; at zero permits it suspends and queues, woken in arrival order (first in, first out) — a real suspension like `lock`, not polling |
| `release()` | Returns one permit: with a waiter present it hands the permit directly to the front of the queue (the count is unchanged), otherwise the count gains one; there is no ownership requirement — `release` without `acquire` is legal, and the count may exceed the initial value (as in Java and Python) |
| `tryAcquire()` | Non-suspending probe: returns `true` and takes a permit when one is available, `false` at zero |

Over-releasing is not an error, but it inflates the count and
defeats the cap — keep `acquire`/`release` paired with the same
discipline as `lock`/`unlock`.

An admission probe — three tasks contend for two permits, and the
overlapping peak is always `2` (the shipped
`examples/conc_semaphore.n` is its four-task variant):

```nlang
import io;
import thread;

class Worker {
    public Semaphore slots;
    public Mutex gate;
    public int inside;
    public int peak;
    public void work() {
        slots.acquire();
        gate.lock();
        inside = inside + 1;
        if (inside > peak) {
            peak = inside;
        }
        gate.unlock();
        thread.yield();
        gate.lock();
        inside = inside - 1;
        gate.unlock();
        slots.release();
    }
}

int main() {
    Worker w = Worker();
    w.slots = Semaphore(2);
    w.gate = Mutex();
    Thread a = thread.spawn(w.work);
    Thread b = thread.spawn(w.work);
    Thread c = thread.spawn(w.work);
    thread.join(a);
    thread.join(b);
    thread.join(c);
    io.write(w.peak.toString());
    return 0;
}
```

## Signal switches and polling

A zero-permit semaphore works as a switch: waiters stop at `acquire()`,
and the event side lets them through with `release()`:

```nlang
import io;
import thread;

class Starter {
    public Semaphore gate;
    public void run() {
        gate.acquire();
        io.write("started");
    }
}

int main() {
    Starter s = Starter();
    s.gate = Semaphore(0);
    Thread t = thread.spawn(s.run);
    thread.sleep(5);
    io.write("go");
    s.gate.release();
    thread.join(t);
    return 0;
}
```

The output is always `gostarted` — `run` cannot
pass its `acquire` before the `release`.

`tryAcquire()` plus `sleep` gives the "not yet — rest and retry" polling
shape — `sleep` is counted in milliseconds, and this is not spinning:
while the permit is absent the task rests inside `sleep` and wakes to
retry once per interval:

```nlang
import io;
import thread;

class Taster {
    public Semaphore pool;
    public void fill() {
        thread.sleep(10);
        pool.release();
    }
    public void poll() {
        while (!pool.tryAcquire()) {
            thread.sleep(2);
        }
        io.write("granted");
    }
}

int main() {
    Taster t = Taster();
    t.pool = Semaphore(0);
    Thread f = thread.spawn(t.fill);
    Thread p = thread.spawn(t.poll);
    thread.join(f);
    thread.join(p);
    return 0;
}
```

`tryAcquire()` never suspends, so it is safe inside re-entrant
evaluation (a `toString` override, for example); a zero-permit
`acquire` is rejected there like a contended `lock` (see [Restrictions
inside re-entrant evaluation](concurrency-suspension.md#restrictions-inside-re-entrant-evaluation)).

## Semaphore versus Channel

A bounded channel also expresses "at most N": once a channel of
capacity N is full, `send` suspends naturally. But a channel hands off
**data** — every `send` carries a value; a semaphore expresses
**admission counting** — `release` carries nothing. When the permit and
the data belong together (a work queue), use a channel; when you only
count slots (a connection pool, a concurrency cap), use a semaphore.
