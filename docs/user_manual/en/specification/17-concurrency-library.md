# 17. The Concurrency Library

The concurrency library is two standard library packages built
on the kernel primitives of chapter 11: the `thread` package
drives execution contexts, and the `concurrent` package
provides `WaitGroup` and `Semaphore`. The semantics of every
operation — suspension, scheduling, failure propagation,
synchronization — are chapter 11; this chapter fixes the
import form and the signature surface.

## 17.1 The thread Package

`thread` is a standard library package (§12.4): `import thread;`
brings it into scope and every function is called through the
package qualifier (`thread.spawn(f)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; every function is a `native` function
(§7.5) whose implementation is built into the virtual machine
itself, not a native library shipped with the toolchain. The
handle classes the package produces — `Thread` and `Future<T>`
— are built-in handle classes (§11.2): their names need no
import.

- `thread.spawn(f)` takes a zero-argument function value
  (§7.4). A `func<void>` delegate yields a `Thread` handle; a
  `func<T>` delegate yields a `Future<T>` handle (§11.5). The
  typed form is bound by the compiler on top of the declared
  base form `Thread spawn(func<void> f)`.
- `thread.join(t)` takes a `Thread` handle.
- `thread.yield()` takes no arguments.
- `thread.id()` returns an `int`.
- `thread.sleep(ms)` takes an `int` count of milliseconds.

The behavior of each function — parking, waking, rethrowing a
failed target, the host-level errors — is §11.1.

## 17.2 The concurrent Package

`concurrent` is a standard library package (§12.4):
`import concurrent;` brings it into scope. Unlike `thread`, it
has no native backing: its two classes are ordinary NLang
classes declared in the package source and built on the kernel
primitives (`thread`, `Mutex`, `Channel`). Their names
therefore require the import, unlike the built-in handle
classes of §11.2.

- `WaitGroup()` constructs a counter with no outstanding work.
  `add(n)` accumulates `n` outstanding units, `done()` retires
  one unit, and `wait()` parks until the count reaches zero.
  The contract and its failure modes are §11.3.
- `Semaphore(initial)` sets the initial permit count.
  `acquire()` takes one permit and `release()` returns one.
  The admission rules are §11.3.

Importing `concurrent` does not bring `thread` into scope
(§3.3): code that spawns work for a `WaitGroup` imports
`thread` itself.
