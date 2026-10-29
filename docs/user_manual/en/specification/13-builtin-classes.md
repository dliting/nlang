# 13. Built-in Classes and Library Conventions

## 13.1 Scope

The implementation provides the built-in classes itself; they need
no import (§12.4). This chapter defines the method surface of the
built-in value and container classes — the `Object` protocol
(§13.2), the `string` methods (§13.3) and the `List` and `Dict`
methods (§13.4) — and the conventions the standard library follows
(§13.5).

Method surfaces defined elsewhere: the concurrency handle classes
(`Mutex`, `Channel`, `Future`, `Thread`) in chapter 11, the stream
handle classes (`ByteStream`, `FileStream`) in chapter 15, and the
exception classes in §9.3.

Every method in this chapter is built in: its declaration lives in the
prelude package `builtins` (§12.4), imported by every build, and a
program cannot replace it — a user class with the same name is an
ordinary class and never takes over the built-in receivers. Two of the
prelude classes, `String` and `Array`, are declaration holders only:
they declare the method surfaces of the `string` keyword and the array
types, cannot be instantiated, extended, or used as types, and stay
outside the class hierarchy — `string` and the bracket array syntax are
the value spellings.

## 13.2 The Object Protocol

Every class inherits three protocol methods from `Object` (§4.9):
`int equals(Object other);`, `int getHashCode();` and
`string toString();`. Chapter 8 defines their virtuality and
overriding (§8.2). The defaults compare object identity: `equals`
yields `1` when receiver and argument denote the same object and
`0` otherwise, and `getHashCode` yields an `int` equal for equal
objects. Neither is reachable through a `null` receiver: a member
access through `null` raises `NullPointerException` (§9.3).

`string` overrides the pair to content: `equals` yields `1`
exactly when both sides hold the same bytes — the same relation as
`==` (§4.4) — and `getHashCode` hashes the bytes, so equal strings
hash equal.

### Default rendering

`toString` renders a value as text. A class that declares no
`toString` renders as the class's source name, `@`, and a
lowercase hexadecimal rendering of an implementation-chosen object
number: `Widget@7f3a`. A `null` receiver raises
`NullPointerException` (§9.3).

Each built-in type renders as follows; `List`, `Dict`, class and
function values also answer `toString()` as a method:

| Receiver | Rendering |
| --- | --- |
| `string` | the string itself |
| `List` | `[e1, e2, ...]` — elements rendered by the rules below |
| `Dict` | `{k1: v1, k2: v2, ...}` — entries rendered by the rules below, in insertion order (§6.4) |
| array | `[...]`, nested per dimension (§4.3, §5.3) |
| enumeration | the member name (§4.5) |
| function value | `func name` or `method name` (§4.11) |
| `Object` holding a boxed value | the value rendered as its type (§4.9) |

Inside a container rendering:

- a `string` element appears quoted, with `\`, `"`, newline,
  carriage return and tab escaped as `\\`, `\"`, `\n`, `\r` and
  `\t`;
- a `null` element appears as `<null>`;
- a `struct` element appears as `<struct>` (structs carry no
  `toString`, §4.6);
- a nested container, array, boxed value or function value renders
  by its own rules.

A rendering nests at most 64 levels deep; a structure nested
deeper — a cycle through `Object` fields — raises a host-level
run-time error at the `toString` site (§9.1).

## 13.3 string Methods

The `string` methods operate on bytes (§4.4): offsets are byte
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
(§4.4) — and `equals`, `getHashCode` and `toString` (§13.2). `equals`
takes a `string` argument; wherever a parameter takes `string`, a null
argument is accepted and reads as the empty string. The
subscript `s[i]` reads byte `i` as a `ubyte` (§5.7).

## 13.4 List and Dict Methods

A `List<T>` (§4.10) is a growable sequence:

| Method | Effect | Failure behavior |
| --- | --- | --- |
| `add(value)` | appends `value` | — |
| `get(i)` | the element at position `i` | out of range raises `IndexOutOfBoundsException` |
| `set(i, value)` | replaces the element at `i` | out of range raises `IndexOutOfBoundsException` |
| `length()` | the element count | — |
| `removeAt(i)` | removes the element at `i`, shifting later elements down | out of range raises `IndexOutOfBoundsException` |
| `indexOf(value)` | the first position matching `value`, `-1` if none | — |
| `contains(value)` | whether some position matches `value` | — |
| `clear()` | removes every element | — |
| `toString()` | `[e1, e2, ...]` (§13.2) | — |

`indexOf` and `contains` match by the kind-aware equality of
§4.10: primitive elements by value, `string` elements by content,
other reference elements by identity.

A `Dict<K, V>` (§4.10) maps keys to values:

| Method | Effect | Failure behavior |
| --- | --- | --- |
| `set(key, value)` | stores `value` under `key`; re-storing a key replaces the value and keeps the key's position | — |
| `get(key)` | the value stored under `key` | no entry for `key` raises `Exception` |
| `containsKey(key)` | whether an entry for `key` exists | — |
| `remove(key)` | removes the entry for `key`; yields whether one was removed | — |
| `clear()` | removes every entry | — |
| `count()` | the entry count | — |
| `keys()` | a fresh `List<K>` of the keys in insertion order (§6.4) | — |
| `toString()` | `{k1: v1, k2: v2, ...}` in insertion order (§13.2) | — |

Key matching is the kind-aware equality of §4.10. The subscript
sugar `li[i]` and `d[k]` maps to `get` and `set` (§5.7); a
container store or retrieval moves values by the categories of
§10.1.

## 13.5 Library Conventions

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
  of §9.3; the library declares no exception classes of its own.
  Chapter 19 maps each library operation to the class it raises.
- Text output renders values through `toString` (§5.3, §13.2);
  the argument surface of the console functions is chapter 15.

> Implementation note. The built-in classes are registered by the
> implementation; the planned direction is to declare them in `.n`
> declaration files shipped as a builtins preset package, making
> those declarations the source of truth (§12.4).
