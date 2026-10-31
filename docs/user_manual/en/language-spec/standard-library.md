# Standard Library


The standard library consists of six packages — `math`, `io`, `fs`,
`thread`, `concurrent`, `tensor` (package-qualified free functions) — plus string
methods (receiver-dispatched). The **prelude package** `builtins`
(`stdlib/builtins.n`) sits beside them: every build imports it
automatically, so its declarations are visible without any import
statement (an explicit `import builtins;` is legal and deduplicates).
It declares the always-visible names as ordinary source — the base
containers `List<T>` and `Dict<K,V>`, the exception family (`Exception`
and its six built-in subclasses), the hierarchy root `Object`, and the
two declaration-holder classes `String` and `Array`: they declare the
method surfaces of the `string` keyword and the array types and nothing
else — they cannot be instantiated, extended, or used as a type, and a
user class with the same name is an ordinary class that never takes
over the keyword or array receivers. Their signatures (constructor
arity, method surface) are exactly what the file declares, and the
language service treats them like any other declaration. The
concurrency classes are ordinary package classes: `thread`
(`stdlib/thread.n`) declares `Thread`, `Future<T>`, `Promise<T>` and
the `thread.*` functions; `concurrent` (`stdlib/concurrent.n`)
declares `Mutex`, `ReentrantMutex`, `Semaphore`, `AtomicInt`,
`Channel<T>`, `ChannelClosedException` and `WaitGroup` — each package
needs its import (see [Concurrency](concurrency.md) and
[Built-in generic classes](builtin-generic-classes.md)). The streams
`ByteStream`/`FileStream` remain import-free (see
[Streams](stdlib-streams.md)). A package is
identified by its file's path — `stdlib/io.n` is the package `io` — and
one build may contain only one package of each name (a duplicate is a
compile error naming both sources; a project directory named `io` is an
ordinary directory). Calls use the qualified name only (`math.sin(x)`);
bare names are not in scope. A package name used as a value
(`int x = math;`) fails to resolve — packages are not values. In
terms of duties, io carries content input/output (IO), math numeric
computation, fs the filesystem's names and metadata, tensor the numeric
array kernels (see [tensor](stdlib-tensor.md)), thread the
virtual-thread spawn and suspension primitives, and concurrent the
coordination classes — `Mutex`, `Channel`, `Semaphore`, `AtomicInt`,
`WaitGroup` (see [Concurrency](concurrency.md)).

The **signatures** (parameter kinds, arity, return type) live in the
`stdlib/*.n` declarations shipped with the toolchain and reach the compiler
and editor (completion, hover, go-to-definition) through the language
service's symbol index. The shape is the one a third-party library uses:
`stdlib/*.n` declares the surface, the `native` members are implemented in
`nlang_<ns>.dll` and reached through the host application binary interface (ABI) at run time (the `thread` and `concurrent` packages are the
exceptions: their `native` declarations are implemented inside the virtual
machine itself and dispatched on the intrinsic channel). A qualified
call type-checks against the declaration and goes through the normal
function-call path; the built-in string and array member methods are
type-checked against their prelude declarations and dispatched directly
into their built-in implementations. See [Libraries and search paths](stdlib-search-paths.md)
for the directory rules.

**Parameter types**: each argument is checked against the declared
parameter type per the conversion rules — same-type arguments pass as-is,
rules-allowed implicit widening is applied automatically (integer-family and `float` arguments
enter `double` parameters, e.g. `math.sqrt(4)`; narrowing is always an
explicit `as` — `math.absi(1.5)` is a compile error). The exceptions are the
three coercing io functions (`write`/`print`/`eprint`), which accept string, arrays,
all scalar primitives and function values
(converted at the call site; a function value renders as `func <name>`,
see [Function Types and Delegates](function-types-and-delegates.md));
class objects and enum variables need an explicit
`.toString()` before printing (an enum member constant prints its member
name directly; struct arguments are rejected outright —
structs have no `toString`).

The detailed reference is one page per topic:

- [math — 25 functions](stdlib-math.md)
- [tensor — numeric array kernels](stdlib-tensor.md)
- [io — content IO](stdlib-io.md) (token reads and mixed semantics included)
- [fs — names, directories, metadata](stdlib-fs.md)
- [Concurrency — virtual threads, Mutex, Channel, Future, and synchronization tools](concurrency.md)
- [string methods — 18 built-ins](stdlib-string.md)
- [Streams — ByteStream and FileStream](stdlib-streams.md)
- [Libraries and search paths](stdlib-search-paths.md) (nide configuration and the third-party library shape included)
- [Exception mapping](stdlib-exceptions.md)
