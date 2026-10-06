# Standard Library


The standard library consists of three packages — `math`, `io`, `fs`
(package-qualified free functions) — plus string methods
(receiver-dispatched). A package is
identified by its file's path — `stdlib/io.n` is the package `io` — and
one build may contain only one package of each name (a duplicate is a
compile error naming both sources; a project directory named `io` is an
ordinary directory). Calls use the qualified name only (`math.sin(x)`);
bare names are not in scope. A package name used as a value
(`int x = math;`) fails to resolve — packages are not values. In
terms of duties, io carries content input/output (IO), math numeric
computation, and fs the filesystem's names and metadata.

The **signatures** (parameter kinds, arity, return type) live in the
`stdlib/*.n` declarations shipped with the toolchain and reach the compiler
and editor (completion, hover, go-to-definition) through the language
service's symbol index. The shape is the one a third-party library uses:
`stdlib/*.n` declares the surface, the `native` members are implemented in
`nlang_<ns>.dll` and reached through the host application binary interface (ABI) at run time. A qualified
call type-checks against the declaration and emits `OP_CallFunc`; the
built-in string methods are receiver-dispatched and emit
`OP_CallIntrinsic`. See [Libraries and search paths](stdlib-search-paths.md)
for the directory rules.

**Parameter types**: each argument is checked against the declared kind
per the conversion matrix — same-kind passes as-is, matrix-allowed implicit
widening is applied automatically (integer-family and `float` arguments
enter `double` parameters, e.g. `math.sqrt(4)`; narrowing is always an
explicit `as` — `math.absi(1.5)` is a compile error). The exceptions are the
io coercing trio (`write`/`print`/`eprint`), which accept string, arrays,
all scalar primitives and function values
(converted at the call site; a function value renders as `func <name>`,
see [Function Types and Delegates](function-types-and-delegates.md));
class and enum values need an explicit
`.toString()` before printing (struct arguments are rejected outright —
structs have no `toString`).

The detailed reference is one page per topic:

- [math — 25 functions](stdlib-math.md)
- [io — content IO](stdlib-io.md) (token reads and mixed semantics included)
- [fs — names, directories, metadata](stdlib-fs.md)
- [string methods — 18 built-ins](stdlib-string.md)
- [Streams — ByteStream and FileStream](stdlib-streams.md)
- [Libraries and search paths](stdlib-search-paths.md) (nide configuration and the third-party library shape included)
- [Exception mapping](stdlib-exceptions.md)
