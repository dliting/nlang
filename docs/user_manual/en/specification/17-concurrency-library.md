# 17. The Concurrency Library

The concurrency library is two standard-library packages whose
semantics chapter 11 fixes: the `thread` package answers who runs
and where the result is fetched, the `concurrent` package answers
how running computations coordinate. The semantics of every
operation — suspension, scheduling, failure propagation,
synchronization — are chapter 11; this chapter fixes the import
form and the signature surface.

## 17.1 The thread Package

`thread` is a standard library package (§12.4): `import thread;`
brings it into scope and every function is called through the
package qualifier (`thread.spawn(f)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; every function is a `native` function
(§7.5) whose implementation is built into the virtual machine
itself, not a native library shipped with the toolchain. The
handle classes the package declares — `Thread`, `Future<T>` and
`Promise<T>` (§11.2, §11.5) — live in the same source and reach
scope through the same import.

- `thread.spawn(f)` takes a zero-argument function value (§7.4).
  A `func<void>` delegate yields a `Thread` handle; a `func<T>`
  delegate yields a `Future<T>` handle (§11.5). The typed form is
  bound by the compiler on top of the declared base form
  `Thread spawn(func<void> f)`.
- `thread.join(t)` takes a `Thread` handle.
- `thread.isFailed(t)` takes a `Thread` handle and returns a
  `bool`; `thread.failure(t)` returns an `Exception` (§11.1).
- `thread.yield()` takes no arguments.
- `thread.id()` returns an `int`.
- `thread.sleep(ms)` takes an `int` count of milliseconds.

The behavior of each function — parking, waking, rethrowing a
failed target, the host-level errors — is §11.1.

## 17.2 The concurrent Package

`concurrent` is a standard library package (§12.4):
`import concurrent;` brings it into scope. Its handle classes —
`Mutex`, `ReentrantMutex`, `Semaphore`, `AtomicInt`,
`Channel<T>` — and the exception subclass
`ChannelClosedException` (§9.3) are declared in the package
source as native classes whose implementations are built into
the virtual machine, like `thread`'s functions; their
constructors and methods are called through instances, not
through a package qualifier. `WaitGroup` is the package's one
ordinary NLang class, built on `Mutex` and `Channel` (§11.3).

Importing `concurrent` does not bring `thread` into scope
(§3.3): code that spawns work for a `WaitGroup` imports
`thread` itself.
