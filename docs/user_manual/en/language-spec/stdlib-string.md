# string methods — 18 built-ins

The normative rules are in [specification §13.2–§13.3](../specification/13-builtin-classes.md).

Methods on the string receiver (`s.substring(1)`; literal receivers work:
`"abc".toUpper()`). Every string method is declared by the prelude class
`String` (`stdlib/builtins.n`). That class exists only to hold the
declarations: it cannot be instantiated, extended, or used as a type —
the value type is written with the `string` keyword. The string method
list is as follows. **Byte
semantics** (Go/Lua model): length, substring
and indexOf are byte offsets; Unicode Transformation Format (UTF-8) byte order equals code point order (so
relational comparison has a definite result); case conversion covers the American Standard Code for Information Interchange (ASCII) only.
Code-point access (`charAt`/`charCount`/`foreach char`) is built on top of
byte access — see [String](string.md) "The char bridge".

| Method | Signature | Notes |
|---------|-----------|-------|
| substring | (start[, end]) → string | end exclusive, defaults to length; out of range → IndexOutOfBoundsException |
| indexOf | (string) → int | first byte offset, -1 if absent |
| startsWith / endsWith / contains | (string) → bool | predicates |
| toUpper / toLower | () → string | ASCII only |
| trim | () → string | strips `" \t\n\r\f\v"` |
| split | (string sep) → List\<string\> | sep must be non-empty (else Exception) |
| replace | (string old, string new) → string | all non-overlapping occurrences; old must be non-empty |
| toInt / toLong | () → int / long | strict whole-string parse; malformed → Exception |
| toFloat / toDouble | () → float / double | strict whole-string parse; malformed → Exception |
| toBool | () → bool | strict parse of `"true"`/`"false"` |
| toChar | () → char | strict decimal code-point parse; invalid scalar → Exception |
| charAt | (int byteIndex) → char | the code point **starting at** that byte offset; out of range → IndexOutOfBoundsException, continuation byte → Exception |
| charCount | () → int | code-point count (contrast `length()`, bytes); invalid sequence → Exception |

Alongside the 18 methods, a string receiver also answers `length()`
(byte count — see [String](string.md)), `equals(other)` (content equality,
the same relation as `==`; yields int `1`/`0`; the argument must be a
`string` or null), `getHashCode()` (content
hash as int — equal strings hash equal) and `toString()` (identity —
returns the string itself). A null argument is accepted wherever a
parameter takes `string` and reads as the empty string — the null-string
model.

String subscripting (`s[i]`) returns byte i as a `ubyte` (out of range →
IndexOutOfBoundsException) — byte access goes through the subscript,
code-point access through `charAt`.
