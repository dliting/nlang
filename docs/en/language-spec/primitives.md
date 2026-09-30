# Primitives


NLang has 12 scalar primitive types: 8 integers, 2 floating-point types,
`bool`, and `char`. A 13th primitive, `string`, is an immutable object with
its own page: [String](string.md).

| Type | Size | Semantics | Literals |
|------|------|-----------|----------|
| `byte` / `ubyte` | 1 byte | s8 / u8 | value-range tiering (below) |
| `short` / `ushort` | 2 bytes | s16 / u16 | value-range tiering |
| `int` / `uint` | 4 bytes | s32 / u32 | value-range tiering |
| `long` / `ulong` | 8 bytes | s64 / u64 | value-range tiering |
| `float` | 4 bytes | IEEE 754 single precision | `1.5f` (the `f` suffix is required) |
| `double` | 8 bytes | IEEE 754 double precision | `1.5`, `2.5e-3` |
| `bool` | 4 bytes | truth value | `true` / `false` |
| `char` | 4 bytes | Unicode scalar value (surrogates excluded) | `'a'`, `'中'`, `'\n'`, `'\u0041'` |

### Numeric literals

**Integer literals tier by value range**: an unsuffixed decimal integer
literal has the narrowest type that holds it — within int32 it is `int`;
above int32 but within int64 it is `long`; positive values above INT64_MAX
are `ulong`; beyond that, a compile error. Hexadecimal literals follow the
same rule (`0xFF` is the `int` 255). Literals **carry no leading sign** —
`-5` is unary minus applied to the literal `5`, which is why
`-9223372036854775808` is legal (negated as a ulong literal, folding back
into the long range).

**Floating-point literals**: unsuffixed decimal and exponent forms are
`double` (`1.5`, `2.5e-3`, `1e5`); `float` requires the `f` suffix
(`1.5f`).

**Constant fit** (the Java/C# rule): an integer **literal** (including
unary-minus forms) assigned to a narrower target type is implicitly legal
when the value is in the target's range — `byte b = 5;` is legal,
`byte b = 1000;` is the compile error `constant 1000 out of range for
'byte'`. The special case covers only the literal itself, not folded
expressions — `byte b = 1 + 2;` still needs an explicit `as byte` (the
type of `1 + 2` is already `int`). The same special case covers a double
literal into a `float` target: `float f = 1.5;` is legal, `float f =
1e50;` is a compile error; integer targets never accept floating-point
constants (`int x = 2e5;` is a compile error).

### bool

`bool` has exactly two values, `true` / `false`. **Producers**: all
comparison operators (`==` `!=` `<` `>` `<=` `>=`), the logical operators
`&&`/`||`/`!`, and every standard-library predicate (`fs.exists`,
`s.startsWith`, `List.contains`, `Dict.containsKey`, ...). **Consumers**:
the five condition positions — the conditions of `if`/`while`/
`do-while`/`for` and the argument of `assert` — plus the operands of
`&&`/`||`/`!` — all accept **bool only**; writing `if (1)` is a compile
error (`if condition must be bool, not "Int32"`). bool converts to and
from no other type and takes no part in arithmetic.

### char

`char` holds one Unicode scalar value (a code point, excluding the
surrogate range U+D800..U+DFFF). Literals use single quotes: ordinary
characters `'a'`, non-ASCII characters `'中'`, escapes (the five `'\n'`,
`'\r'`, `'\t'`, `'\''`, `'\\'`), and the `\uXXXX` escape
`'\u0041'` (i.e. `'A'`). A surrogate-range escape inside a char literal is a compile
error; inside a string literal, adjacent surrogate escapes combine into
one code point (`"😀"` is U+1F600 😀, as in Java).

There is **no implicit conversion** between char and the numeric types:

- Arithmetic with a `char` operand (`c + 1`) is a compile error — take the
  code point first with `c as int`.
- char compares only with char (`'a' < 'b'` is legal and yields bool);
  comparing against a number needs an explicit `as int` first.
- `65 as char` constructs a char explicitly (the scalar value is validated
  at run time; an invalid value throws a runtime error); `c as int` reads
  the code point back.

The char↔string bridge (`"x" + 'y'` concatenation, `foreach (char c in s)`
code-point iteration, the `charAt`/`charCount` methods) is on
[String](string.md).

### Representation: from source to console

One representation question spans the whole chain — what a char is in
the source file, after compilation, and on the console:

- **Source files are UTF-8.** The scanner decodes a character literal's
  1-4 byte UTF-8 span into one code point (`'中'` is the 3 bytes
  `E4 B8 AD`, decoded to U+4E2D). A file must not start with a UTF-8
  byte-order mark: the BOM is not recognized and silently corrupts the
  first token (a BOM-prefixed `int main()...` compiles "successfully"
  but yields no main function). Save sources as UTF-8 **without** BOM.
- **After compilation a char is its 32-bit code point — neither UTF-8
  nor UTF-16.** Every char lives in a 4-byte slot holding the raw code
  point: frame slots, the 4 immediate bytes of a char literal, and the
  4-byte value in boxed records and stream writes. The module format
  marks the kind in one byte (`char` is kind 19), while the value
  stays the plain code point end to end.
- **Strings are where UTF-8 lives**: a string object is a UTF-8 byte
  sequence (see [String](string.md) "Encoding and length"). A char
  converts to that form on joining a string — `"x" + 'y'` encodes the
  code point as its 1-4 UTF-8 bytes. In the other direction,
  `s.charAt(i)` decodes the code point starting at byte i, and
  `"65".toChar()` parses a decimal code point.
- **The console receives UTF-8 bytes.** `io.print` writes a char as
  its UTF-8 encoding and a string as its raw bytes, verbatim, to
  standard output. The tools do not switch the console's code page, so
  on Windows non-ASCII output needs a UTF-8 terminal — Windows
  Terminal, or `chcp 65001` in the classic console.

### Arithmetic

```nlang
a + b    a - b    a * b    a / b    a % b
```

Integer division truncates toward zero. Division/modulo by zero throws a
runtime error — this includes floating-point division by zero, which
throws rather than producing IEEE 754 ±inf/NaN (NLang diverges from
C/C++/Java here). `%` on floating-point operands takes the fmod remainder
(`7.5 % 2.0` = 1.5).

**Integer overflow** wraps silently in two's complement (C-style):
`INT_MAX + 1 == INT_MIN`. There is no SafeInt-style checking. Lock-in test:
`tests/e2e/int_overflow_wrap.n`.

**Numeric promotion**: integer operands narrower than int are first
promoted to int (`byte + byte` has type `int`); the result type is then
**the smallest type that can implicitly receive both operands**:

- `int + int` → `int`; `byte + byte` → `int`
- `int + uint` → `long` (int32 and u32 both fit in s64)
- `long + uint` → `long`
- mixing `int`/`long` with `ulong` → **compile error** (`no implicit
  common type`) — unify the sign domain explicitly first
- when floating point participates, the larger float rank wins:
  `int + float` / `float + int` → `float` (symmetric); any `double`
  operand → `double`

So `1 + 2.5 == 2.5 + 1 == 3.5` (symmetric). The result type is the
promoted type; assigning back to a narrower type requires an explicit `as`
(constant fit excepted, see above). `string + string` is concatenation
(see [String](string.md)); `string - string` and the other arithmetic
forms are compile errors.

### Comparison

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

**Comparison results are always `bool`.** Numeric operands get the same
symmetric promotion as arithmetic: `-2 < -1.5` promotes to double and is
true; `1 == 1.0` is true. char compares only with char. The complete
comparison rules for all types (string, class, null) are on
[Operators](operators.md) "Comparison".

### Casts

The conversion matrix between the integer family, floating point, `bool`,
and `char` — which conversions are implicit, which require an explicit
`as`, which are forbidden — is on [Type Casts](type-casts.md).
