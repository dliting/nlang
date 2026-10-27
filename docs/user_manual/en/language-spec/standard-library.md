# Standard Library


The standard library consists of six packages — `math`, `io`, `fs`,
`thread`, `concurrent`, `tensor` (package-qualified free functions) — plus string
methods (receiver-dispatched). The **prelude package** `builtins`
(`stdlib/builtins.n`) sits beside them: every build imports it
automatically, so its declarations are visible without any import
statement (an explicit `import builtins;` is legal and deduplicates).
It declares the compiler's non-generic built-in classes as ordinary
source — the handle classes `Mutex`, `Semaphore`, `Thread`, the
exception family (`Exception` and its seven built-in subclasses), and
the hierarchy root `Object`. Their signatures (constructor arity,
method surface) are exactly what the file declares, and the language
service treats them like any other declaration. The generic containers
`List<T>`/`Dict<T>`/`Channel<T>`/`Future<T>` and the streams
`ByteStream`/`FileStream` are not declared there — they remain
compiler built-ins (see
[Built-in generic classes](builtin-generic-classes.md) and
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
`WaitGroup` synchronization tool (the handle classes are not part of
any function package — see the prelude package `builtins` above and
[Concurrency](concurrency.md)).

The **signatures** (parameter kinds, arity, return type) live in the
`stdlib/*.n` declarations shipped with the toolchain and reach the compiler
and editor (completion, hover, go-to-definition) through the language
service's symbol index. The shape is the one a third-party library uses:
`stdlib/*.n` declares the surface, the `native` members are implemented in
`nlang_<ns>.dll` and reached through the host application binary interface (ABI) at run time (the `thread` package is the one
exception: its `native` declarations are implemented inside the virtual
machine itself and dispatched on the intrinsic channel). A qualified
call type-checks against the declaration and goes through the normal
function-call path; the built-in string methods are receiver-dispatched
directly into their built-in implementations. See [Libraries and search paths](stdlib-search-paths.md)
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
