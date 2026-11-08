# 11. Library Conventions

## 11.1 Scope and Package Surface

The standard library is seven packages — `math`, `io`, `fs`,
`thread`, `concurrent`, `tensor` and `net`, each a set of
package-qualified free functions — plus the `string` methods,
dispatched on the receiver.
The prelude packages sit beside them: `lang` (`stdlib/lang.n`)
declares the `Exception` family, the hierarchy root `Object`, the
declaration-holder class `String` and the generic array face `Array<T>`; `containers`
(`stdlib/containers.n`) declares the base containers `List<T>` and
`Dict<K,V>`. Every build imports both automatically, so their
declarations need no import statement, and an explicit `import lang;`
or `import containers;` is legal and deduplicates (§3.9.4). The
always-visible names they declare — `Object`, the `Exception`
family, `String`, `Array`, `List<T>` and `Dict<K,V>` — are the whole
set (§3.9.4).

Every method of the built-in value and container classes is built
in: its declaration lives in a prelude package, imported by every
build, and a program cannot replace it — a user class with the same
name is an ordinary class and never takes over the built-in
receivers. The `String` prelude class is a
declaration holder only: it declares the method surface of the
`string` keyword, cannot be instantiated, extended, or used as a
type, and stays outside the class hierarchy — the `string` keyword
is the value spelling. The `Array` prelude class went the other
way: `Array<T>` is a real generic type (§4.10.1) whose values are
constructed with the bracket creation forms, while the class
itself only holds the array method declarations and cannot be
extended.

The method surfaces are defined across the specification: the
`string` methods in chapter 17, the `List` and `Dict` methods in
chapter 12, the default rendering of every value in §11.2, the
concurrency handle classes and the package classes of `thread` and
`concurrent` in chapter 10 and chapter 18, the stream handle
classes in chapter 14, and the exception classes in §8.3.

A package is identified by its file's path — `stdlib/io.n` is the
package `io` — and one build may contain only one package of each
name: a duplicate is a compile error naming both sources, and a
project directory named `io` is an ordinary directory. Calls use
the qualified name only (`math.sin(x)`); bare names are not in
scope. A package name used as a value (`int x = math;`) fails to
resolve — packages are not values. In terms of duties, `io` carries
content input/output (IO), `math` numeric computation, `fs` the
filesystem's names and metadata, `tensor` the numeric array kernels
(chapter 19), `net` the network sockets (chapter 21), `thread` the
virtual-thread spawn and suspension primitives, and `concurrent`
the coordination classes (§3.9.4).

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

The `string` method surface is defined in chapter 17.

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
  of §8.3, plus the three package-declared classes — `concurrent`'s
  `ChannelClosedException` and `net`'s `SocketException` and
  `TimeoutException` (§8.3). Chapter 20 maps each library operation
  to the class it raises.
- Text output renders values through `toString` (§5.3, §11.2);
  the argument surface of the console functions is chapter 14.
- Arguments of a library call are checked against the declared
  parameter type per the conversion rules: same-type arguments
  pass as-is, rules-allowed implicit widening is applied
  automatically — integer-family and `float` arguments enter
  `double` parameters, so `math.sqrt(4)` compiles — and narrowing
  is always an explicit `as` (§4.4, §7.2). The primitive-to-string
  coercion of §4.5 does not fire at a package call: a library
  `string` parameter takes `string` only. The three coercing `io`
  functions (`write`, `print`, `eprint`) are the one exception
  (§14.2).
