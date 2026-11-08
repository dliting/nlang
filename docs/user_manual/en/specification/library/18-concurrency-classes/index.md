# 18. The Concurrency Classes

The concurrency library is two standard-library packages whose
semantics chapter 10 fixes: the `thread` package answers who runs
and where the result is fetched, the `concurrent` package answers
how running computations coordinate. The semantics of every
operation — suspension, scheduling, failure propagation,
synchronization — are chapter 10; this chapter fixes the import
form, the declaration site and the signature surface, and gives
each class its own page. §18.1 and §18.2 fix the two package
surfaces; §18.3–§18.11 define the classes, one page each.

## 18.1 The thread Package

`thread` is a standard library package (§3.9.4): `import thread;`
brings it into scope and every function is called through the
package qualifier (`thread.spawn(f)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source at `stdlib/thread.n`; every function is a
`native` function (§7.5) whose implementation is built into the
virtual machine itself, not a native library shipped with the
toolchain. The handle classes the package declares — `Thread`,
`Future<T>` and `Promise<T>` (§10.1, §10.5) — live in the same
source and reach scope through the same import. A user package
named `thread` collides with the standard library and is rejected
by the duplicate-package rule (§11.1).

The package's function surface:

- `thread.spawn(f)` takes a zero-argument function value (§7.4).
  A `func<void>` delegate yields a `Thread` handle; a `func<T>`
  delegate yields a `Future<T>` handle (§10.5). The typed form is
  bound by the compiler on top of the declared base form
  `Thread spawn(func<void> f)`.
- `thread.join(t)` takes a `Thread` handle.
- `thread.isFailed(t)` takes a `Thread` handle and returns a
  `bool`; `thread.failure(t)` returns an `Exception` (§10.1).
- `thread.yield()` takes no arguments.
- `thread.id()` returns an `int`.
- `thread.sleep(ms)` takes an `int` count of milliseconds.

The behavior of each function — parking, waking, rethrowing a
failed target, the host-level errors — is §10.1; the usage and
examples are §18.3.

## 18.2 The concurrent Package

`concurrent` is a standard library package (§3.9.4):
`import concurrent;` brings it into scope. Its declarations are
the standard library's `.n` source at `stdlib/concurrent.n`. Its
handle classes — `Mutex`, `ReentrantMutex`, `Semaphore`,
`AtomicInt`, `Channel<T>` — and the exception subclass
`ChannelClosedException` (§8.3) are declared in the package
source as native classes whose implementations are built into the
virtual machine, like `thread`'s functions; their constructors
and methods are called through instances, not through a package
qualifier.

`WaitGroup` is the package's one ordinary NLang class — not a
virtual-machine handle — built on `Mutex` and `Channel`
(§10.2, §10.4). It is a counting barrier: `add(n)` accumulates
outstanding units, each unit calls `done()` once, and `wait()`
parks until the count reaches zero. The contract is `add` before
`wait` and `done` paired with `add`: an unmatched `done` releases
`wait` early, and a missing `done` parks it forever — the program
ends under the exit policy of §10.1.

Importing `concurrent` does not bring `thread` into scope
(§3.3): code that spawns work for a `WaitGroup` imports
`thread` itself. The class contracts are §18.6–§18.11.
