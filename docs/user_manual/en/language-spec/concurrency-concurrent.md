# The concurrent Package

## When to use

`Semaphore(n)` caps how many tasks run a section at once — resource
pools, connection limits: acquire a permit before entering, release it
on the way out, and keeping the pair matched keeps the count honest.
`WaitGroup` waits for a fan-out batch: `add` the unit count before
spawning the tasks, each task calls `done` once as it finishes, and
`wait` returns when the batch is complete — without holding any
`Thread` handles.

## The tools

On top of the kernel primitives, the `concurrent` standard-library package provides two common synchronization tools (available after `import concurrent;`):

| Class | Shape | Semantics |
|---|---|---|
| `WaitGroup` | `add(n)` / `done()` / `wait()` | A counting barrier (Go-shaped): `add` accumulates outstanding units, each unit calls `done` once, and `wait` suspends until the count reaches zero. Contract: `add` before `wait`, and `done` paired with `add` — an unmatched `done` releases `wait` early, a missing `done` suspends it until the deadlock report |
| `Semaphore` | `Semaphore(n)` / `acquire()` / `release()` | A counting semaphore: `acquire` takes a permit (yielding while the count is zero), `release` returns one |

```nlang
import concurrent;
import io;
import thread;

class Job {
    public WaitGroup wg;
    public Mutex gate;
    public int total;
    public void work() {
        gate.lock();
        total = total + 10;
        gate.unlock();
        wg.done();
    }
}

int main() {
    Job j = Job();
    j.gate = Mutex();
    j.wg = WaitGroup();
    j.wg.add(4);
    Thread a = thread.spawn(j.work);
    Thread b = thread.spawn(j.work);
    Thread c = thread.spawn(j.work);
    Thread d = thread.spawn(j.work);
    j.wg.wait();
    io.write(j.total.toString());
    thread.join(a);
    thread.join(b);
    thread.join(c);
    thread.join(d);
    return 0;
}
```

`wait` is a real barrier — reading `40` holds exactly when all four increments landed before the read. `Semaphore` currently waits by yielding (the kernel has no condition-variable primitive yet): an `acquire` against a zero count takes turns via `thread.yield` instead of suspension, without monopolizing a carrier.
