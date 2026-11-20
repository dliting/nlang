# 17. string Methods

The `string` methods are the built-in operations dispatched on
a `string` receiver: search, slicing, case conversion, parsing
and code-point access. §17.1 fixes the receiver and its byte
semantics; §17.2 defines the nineteen methods.

## 17.1 Receiver and Byte Semantics

A method call on a `string` receiver — `s.substring(1)` — works
on any `string` expression, a literal included
(`"abc".toUpper()`). Every string method is declared by the
prelude class `String` (§11.1): the class exists only to hold
the declarations, cannot be instantiated, extended, or used as
a type, and the value type is written with the `string` keyword
(§4.3).

The prelude class `String` also carries one constant member:
`string.EMPTY`, the named form of the empty string `""`.

The methods operate on bytes (§4.3): offsets are byte offsets,
and case conversion touches American Standard Code for Information Interchange (ASCII)
letters only. Unicode Transformation Format (UTF-8) byte
order equals code point order, so the relational comparison
of §4.3 has a definite result on UTF-8 content. The
code-point layer — `charAt` and `charCount` — decodes UTF-8
over the byte core: code-point access (`charAt`,
`charCount`, `foreach` over a string) is built on top of
byte access, as "The char bridge" of §4.3 describes.

## 17.2 Methods

The `string` receiver answers the following nineteen methods.

| Method | Signature | Failure behavior |
| --- | --- | --- |
| `substring(start[, end])` | → `string` | the bytes `[start, end)`; `end` defaults to `length()`; `start` or `end` outside `[0, length()]`, or `end < start`, raises `IndexOutOfBoundsException` |
| `indexOf(t[, from])` | → `int` | the first byte offset of `t` at or after `from`, `-1` if absent; `from` defaults to `0`, a negative `from` counts as `0`, a `from` past the end yields `-1` (even for an empty `t`); an empty `t` otherwise matches at `max(from, 0)` |
| `equalsIgnoreCase(t)` | → `bool` | ASCII-only case folding: only `A-Z` fold to `a-z`; every other byte must match exactly |
| `startsWith(t)` / `endsWith(t)` / `contains(t)` | → `bool` | — |
| `toUpper()` / `toLower()` | → `string` | ASCII letters only |
| `trim()` | → `string` | strips leading and trailing space, tab, newline, carriage return, form feed and vertical tab |
| `split(sep)` | → `List<string>` | an empty `sep` raises `Exception` |
| `replace(old, repl)` | → `string` | every non-overlapping occurrence, left to right; an empty `old` raises `Exception` |
| `toInt([radix])` / `toLong()` | → `int` / `long` | strict whole-string parse; `radix` defaults to `10` and must be in `2..36`; a `0x` prefix is not accepted; a malformed string, a `radix` outside `2..36`, or overflow raises `Exception` |
| `toFloat()` / `toDouble()` | → `float` / `double` | strict whole-string parse; a malformed string raises `Exception` |
| `toBool()` | → `bool` | exactly `"true"` or `"false"`; anything else raises `Exception` |
| `toChar()` | → `char` | a decimal code-point number; a value outside the Unicode scalar values raises `Exception` |
| `charAt(byteIndex)` | → `char` | the code point starting at that byte offset; out of range raises `IndexOutOfBoundsException`, a continuation byte raises `Exception` |
| `charCount()` | → `int` | the code-point count; an invalid byte sequence raises `Exception` |

A `string` receiver also answers `length()` — the byte count
(§4.3) — and the protocol methods `equals`, `getHashCode` and
`toString` (§4.11.2, §11.2): `equals` compares content, the
same relation as `==`, and takes a `string` argument;
`getHashCode` hashes the content, so equal strings hash equal.
Wherever a parameter takes `string`, a null argument is
rejected — null is not a string value (§4.3). The subscript
`s[i]` reads byte `i` as a `ubyte` (§5.7).
