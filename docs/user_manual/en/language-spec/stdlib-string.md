# string methods — 18 built-ins

Methods on the string receiver (`s.substring(1)`; literal receivers work:
`"abc".toUpper()`). The string method list is as follows. **Byte
semantics** (Go/Lua model): length, substring
and indexOf are byte offsets; Unicode Transformation Format (UTF-8) byte order equals code point order (so
relational comparison is well-defined); case conversion is American Standard Code for Information Interchange (ASCII)-only.
Code-point access (`charAt`/`charCount`/`foreach char`) is a code-point layer
over the byte core — see [String](string.md) "The char bridge".

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

String subscripting (`s[i]`) returns byte i as a `ubyte` (out of range
throws the runtime error `string index out of range`) — byte access goes
through the subscript, code-point access through `charAt`.
