# 11. Library Conventions

## 11.1 Scope and Package Surface

The standard library is six packages — `math`, `io`, `fs`, `thread`,
`concurrent` and `tensor`, each a set of package-qualified free
functions — plus the `string` methods, dispatched on the receiver.
The prelude package `builtins` sits beside them: every build imports
it automatically, so its declarations need no import statement, and
an explicit `import builtins;` is legal and deduplicates (§3.9.4).
The always-visible names it declares — `Object`, the `Exception`
family, `String`, `Array`, `List<T>` and `Dict<K,V>` — are the whole
set (§3.9.4).

Every method of the built-in value and container classes is built
in: its declaration lives in the prelude package, imported by every
build, and a program cannot replace it — a user class with the same
name is an ordinary class and never takes over the built-in
receivers. Two of the prelude classes, `String` and `Array`, are
declaration holders only: they declare the method surfaces of the
`string` keyword and the array types, cannot be instantiated,
extended, or used as types, and stay outside the class hierarchy —
`string` and the bracket array syntax are the value spellings.

The method surfaces are defined across the specification: the
`string` methods in §11.3, the `List` and `Dict` methods in
chapter 12, the default rendering of every value in §11.2, the
concurrency handle classes and the package classes of `thread` and
`concurrent` in chapter 10 and chapter 17, the stream handle
classes in chapter 15, and the exception classes in §8.3.

A package is identified by its file's path — `stdlib/io.n` is the
package `io` — and one build may contain only one package of each
name: a duplicate is a compile error naming both sources, and a
project directory named `io` is an ordinary directory. Calls use
the qualified name only (`math.sin(x)`); bare names are not in
scope. A package name used as a value (`int x = math;`) fails to
resolve — packages are not values. In terms of duties, `io` carries
content input/output (IO), `math` numeric computation, `fs` the
filesystem's names and metadata, `tensor` the numeric array kernels
(chapter 18), `thread` the virtual-thread spawn and suspension
primitives, and `concurrent` the coordination classes (§3.9.4).

The signatures — parameter kinds, arity and return type — live in
the `stdlib/*.n` declarations shipped with the toolchain and reach
the compiler and editor (completion, hover, go-to-definition)
through the language service's symbol index (§3.9.4). The shape is
the one a third-party library uses: the `.n` source declares the
surface, the `native` members are implemented in `nlang_<ns>.dll`
and reached through the host application binary interface (ABI) at
run time. The `thread` and `concurrent` packages are the exception:
their `native` declarations are implemented inside the virtual
machine itself and dispatched on the intrinsic channel. A qualified
call type-checks against the declaration and goes through the
normal function-call path; the built-in string and array member
methods type-check against their prelude declarations and dispatch
directly into their built-in implementations.

## 11.2 Default Rendering

A class that declares no `toString` renders as §4.11.2 describes:
the class's source name, `@`, and a lowercase hexadecimal rendering
of an implementation-chosen object number (`Point@1a`). A `null`
receiver raises `NullPointerException` (§4.8.10, §8.3).

Each built-in type renders as follows; `List`, `Dict`, class and
function values also answer `toString()` as a method:

| Receiver | Rendering |
| --- | --- |
| `string` | the string itself |
| `List` | `[e1, e2, ...]` — elements rendered by the rules below |
| `Dict` | `{k1: v1, k2: v2, ...}` — entries rendered by the rules below, in insertion order (§12.2) |
| array | `[...]`, nested per dimension (§4.10, §5.3) |
| enumeration | the member name (§4.6) |
| function value | `func name` or `method name` (§7.4) |
| `Object` holding a boxed value | the value rendered as its type (§4.11) |

Inside a container rendering:

- a `string` element appears quoted, with `\`, `"`, newline,
  carriage return and tab escaped as `\\`, `\"`, `\n`, `\r` and
  `\t`;
- a `null` element appears as `<null>`;
- a `struct` element appears as `<struct>` (structs carry no
  `toString`, §4.7);
- a nested container, array, boxed value or function value renders
  by its own rules.

A rendering nests at most 64 levels deep; a structure nested
deeper — a cycle through `Object` fields — raises a host-level
run-time error at the `toString` site (§8.1).

## 11.3 string Methods

The `string` methods operate on bytes (§4.3): offsets are byte
offsets, and case conversion touches
American Standard Code for Information Interchange (ASCII)
letters only. The code-point layer — `charAt` and `charCount` —
decodes Unicode Transformation Format (UTF-8) over the byte core.

| Method | Signature | Failure behavior |
| --- | --- | --- |
| `substring(start[, end])` | → `string` | the bytes `[start, end)`; `end` defaults to `length()`; `start` or `end` outside `[0, length()]`, or `end < start`, raises `IndexOutOfBoundsException` |
| `indexOf(t)` | → `int` | the first byte offset of `t`, `-1` if absent; an empty `t` yields `0` |
| `startsWith(t)` / `endsWith(t)` / `contains(t)` | → `bool` | — |
| `toUpper()` / `toLower()` | → `string` | ASCII letters only |
| `trim()` | → `string` | strips leading and trailing space, tab, newline, carriage return, form feed and vertical tab |
| `split(sep)` | → `List<string>` | an empty `sep` raises `Exception` |
| `replace(old, repl)` | → `string` | every non-overlapping occurrence, left to right; an empty `old` raises `Exception` |
| `toInt()` / `toLong()` | → `int` / `long` | strict whole-string parse; a malformed string raises `Exception` |
| `toFloat()` / `toDouble()` | → `float` / `double` | strict whole-string parse; a malformed string raises `Exception` |
| `toBool()` | → `bool` | exactly `"true"` or `"false"`; anything else raises `Exception` |
| `toChar()` | → `char` | a decimal code-point number; a value outside the Unicode scalar values raises `Exception` |
| `charAt(byteIndex)` | → `char` | the code point starting at that byte offset; out of range raises `IndexOutOfBoundsException`, a continuation byte raises `Exception` |
| `charCount()` | → `int` | the code-point count; an invalid byte sequence raises `Exception` |

A `string` receiver also answers `length()` — the byte count
(§4.3) — and `equals`, `getHashCode` and `toString` (§11.2). `equals`
takes a `string` argument; wherever a parameter takes `string`, a null
argument is accepted and reads as the empty string. The
subscript `s[i]` reads byte `i` as a `ubyte` (§5.7).

## 11.4 Library Conventions

The standard library follows one surface convention.

- Types are PascalCase; methods, functions and variables are
  camelCase; enumeration members are PascalCase; type parameters
  are a single uppercase letter; `main` is lowercase (§3.2).
- A pure accessor may carry a `get` prefix (`getHashCode`); the
  prevailing style is the bare name (`length`, `count`, `keys`).
  Methods of the built-in classes name predicates bare
  (`contains`, `startsWith`, `containsKey`); package functions
  may carry an `is` or `has` prefix (`fs.isFile`, `io.hasInput`).
- Failures are reported by raising the built-in exception classes
  of §8.3, plus the one package-declared class, `concurrent`'s
  `ChannelClosedException` (§8.3). Chapter 19 maps each library
  operation to the class it raises.
- Text output renders values through `toString` (§5.3, §11.2);
  the argument surface of the console functions is chapter 15.
- Arguments of a library call are checked against the declared
  parameter type per the conversion rules: same-type arguments
  pass as-is, rules-allowed implicit widening is applied
  automatically — integer-family and `float` arguments enter
  `double` parameters, so `math.sqrt(4)` compiles — and narrowing
  is always an explicit `as` (§4.4, §7.2). The primitive-to-string
  coercion of §4.5 does not fire at a package call: a library
  `string` parameter takes `string` only. The three coercing `io`
  functions (`write`, `print`, `eprint`) are the one exception
  (§15.2).
