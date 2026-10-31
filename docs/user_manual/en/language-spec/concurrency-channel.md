# Channel

## When to use

Reach for a `Channel` when tasks hand values to each other: a
producer/consumer pipeline, a work queue, or `close()` as one broadcast
that wakes a whole group of waiters. A bounded channel is backpressure
for free — when the producer outruns the consumer, `send` suspends until a
slot frees, so the pipeline throttles itself without extra machinery.
This is the other half of the rule of thumb stated on the
[Mutex](concurrency-mutex.md) page: data you hand over → `Channel`;
state you share → `Mutex`.

## Construction and buffering

`Channel<T>` is a message channel. The element type argument is not optional (a bare `Channel` is a compile error — the element type drives value boxing and receiver-side unboxing). Two construction forms:

```nlang
import concurrent;
Channel<int> a = {};         // unbounded: no buffer limit, send never suspends
Channel<int> b = Channel(2); // bounded: capacity 2, a full buffer suspends send
```

The capacity constructor takes exactly one `int`: `0` is equivalent to unbounded, `n >= 1` is a bounded capacity, and a negative value is a runtime error (the message states the accepted range); the empty initializer `{}` is the unbounded default. On a bounded channel, `send` suspends in a first-in-first-out queue when the buffer is full; a `recv` frees a slot and wakes the head sender — earlier sends enter the buffer first, and order is preserved.

| Member | Semantics |
|---|---|
| `send(v)` | Never suspends on an unbounded channel; on a bounded channel suspends in queue when the buffer is full, resuming in arrival order as slots free |
| `recv()` | Takes the head when the buffer is non-empty; otherwise suspends until a `send` or `close` |
| `close()` | Closes the channel; idempotent, values already in the buffer are not lost |

Closing semantics are drawn by the catchable `ChannelClosedException`: after `close`, values already in the buffer are still delivered by `recv` (sent values outrank the closing signal); once drained, a further `recv` — and any `send` after closing — raises `ChannelClosedException` (a subclass of `Exception`, catchable for recovery). Suspended waiters see the same boundary: `close` first drains the buffer into them, then wakes the remaining receivers and every suspended sender with that exception. When you use "closed" as a broadcast signal, the receiving side finishes with `try`/`catch`:

```nlang
import io;
import thread;
import concurrent;

class Reader {
    public Channel<int> ch;
    public int total;
    public void drain() {
        bool open = true;
        while (open) {
            try {
                total = total + ch.recv();
            } catch (ChannelClosedException e) {
                open = false;
            }
        }
    }
}

int main() {
    Reader r = Reader();
    r.ch = Channel(4);
    Thread t = thread.spawn(r.drain);
    r.ch.send(10);
    r.ch.send(20);
    r.ch.close();
    thread.join(t);
    io.write(r.total.toString());
    return 0;
}
```

Primitive elements are boxed on send and unboxed on receive (the same mechanism as `List<T>`); reference elements pass through directly. The value `0` is a legitimate boxed value, constructively distinguishable from "no value" — the closing boundary is expressed by the exception, not by spending a value.

When the sender arrives first, `recv` takes straight from the buffer; when the receiver arrives first it suspends, and the `send` appends and wakes it:

```nlang
import io;
import thread;
import concurrent;

class Peer {
    public Channel<string> ch;
    public void shout() {
        ch.send("a");
        io.write("w");
    }
}

int main() {
    Peer p = Peer();
    p.ch = {};
    Thread t = thread.spawn(p.shout);
    io.write(p.ch.recv());
    thread.join(t);
    return 0;
}
```

Main suspends on `recv` first; the worker's `send` delivers the value and wakes main, but the worker itself does not suspend — it writes `w` immediately, then main resumes and writes the value it received. The output is `wa`: the value has flowed from sender to receiver.
