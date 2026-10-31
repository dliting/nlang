# The concurrent Package

The normative rules are in [specification §11.3](../specification/11-concurrency.md)
and [§17.2](../specification/17-concurrency-library.md).

## When to use

`WaitGroup` waits for a fan-out batch: `add` the unit count before
spawning the tasks, each task calls `done` once as it finishes, and
`wait` returns when the batch is complete — without holding any
`Thread` handles. `Semaphore`, which caps how many tasks run a section
at once, is a handle class of this same package; see
[Semaphore](concurrency-semaphore.md).

## The tools

One import, `import concurrent;`, brings the whole coordination side
into scope: the handle classes `Mutex`, `ReentrantMutex`, `Semaphore`,
`AtomicInt` and `Channel<T>`, the exception subclass
`ChannelClosedException`, and `WaitGroup` — the package's one ordinary
NLang class, a counting barrier (Go-shaped): `add(n)`
accumulates outstanding units, each unit calls `done()` once, and
`wait()` suspends until the count reaches zero. Contract: `add` before
`wait`, and `done` paired with `add` — an unmatched `done` releases
`wait` early, a missing `done` suspends it until the deadlock report.
Each handle class has its own page (see
[Concurrency](concurrency.md) for the map).

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

`wait` is a real barrier — reading `40` holds exactly when all four increments landed before the read.
