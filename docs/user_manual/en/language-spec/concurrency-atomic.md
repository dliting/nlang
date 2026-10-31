# AtomicInt

## When to use

`AtomicInt` carries cross-carrier shared **single-value** state —
counters, flags, sequence numbers. Field reads and writes are not
locked (see [Carriers and Parallelism](concurrency-carriers.md)); an
ordinary shared-field update can read a transitional value.
`AtomicInt` turns "read-modify-write" into one indivisible step,
without a mutex.

Choosing: **atomic updates to one value** take `AtomicInt`; an
**invariant across several fields** takes [Mutex](concurrency-mutex.md)
(atomicity covers each value independently — the pairing of
`x = x + 1` with `y = y + 2` still needs the mutex); composed or
recursive locking takes
[ReentrantMutex](concurrency-reentrant-mutex.md).

## Operations

The naming follows Java's `AtomicInteger`: `getAndX` returns the
**previous** value, `xAndGet` the **new** one.

| Member | Semantics |
|---|---|
| `AtomicInt()` | initial value 0 |
| `AtomicInt(int initial)` | initial value `initial` |
| `load()` | atomic read, returns the current value |
| `store(int v)` | atomic write |
| `getAndSet(int v)` | atomically sets `v`, returns the previous value |
| `compareExchange(int expect, int v)` | compare-and-swap (CAS): if the current value equals `expect`, sets `v` and returns `true`; otherwise leaves the value and returns `false` |
| `getAndAdd(int delta)` | atomic add, returns the pre-add value |
| `addAndGet(int delta)` | atomic add, returns the post-add value |
| `getAndIncrement()` / `incrementAndGet()` | the two faces of +1 (old / new) |

No operation **ever parks** — all stay legal inside a re-entrant
evaluation (a `toString` override, a host `call()`), with the same
standing as `tryLock()`. Overflow wraps at int32 (plain platform
addition).

## Counter

```nlang
import thread;
import io;
import concurrent;

class Bumper {
    public AtomicInt counter;

    public void bump() {
        int i = 0;
        while (i < 2500) {
            counter.addAndGet(1);   //read-modify-write in one step
            i = i + 1;
        }
    }
}

int main() {
    Bumper b = Bumper();
    b.counter = AtomicInt(0);
    Thread a = thread.spawn(b.bump);
    Thread c = thread.spawn(b.bump);
    thread.join(a);
    thread.join(c);
    io.print(b.counter.load());   //exactly 5000
    return 0;
}
```

Four tasks incrementing concurrently across four carriers land on the
exact total — the field form `counter.n = counter.n + 1` loses updates
under the same interleaving.

## Flag (exactly once)

`compareExchange` is the one atomic "read-judge-write" composite — the
natural shape for run-once initialization:

```nlang
if (flag.compareExchange(0, 1)) {
    //only the first task to flip 0 → 1 enters here
    init();
}
```

## Division of labor with Mutex

| Shape | Use |
|---|---|
| single-value counter / flag / sequence | `AtomicInt` |
| an invariant across several fields | [Mutex](concurrency-mutex.md) |
| composed or recursive locking | [ReentrantMutex](concurrency-reentrant-mutex.md) |
| capping concurrent entry | [Semaphore](concurrency-semaphore.md) |

`AtomicInt` coexists with the mutexes: the hot counter goes atomic,
multi-field state goes under a mutex, each guarding its own shape.
