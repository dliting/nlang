# Operators

The normative rules are in [specification §5.2 and §5.3](../specification/05-expressions.md).

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
`INT_MAX + 1 == INT_MIN`. There is no SafeInt-style checking.

**Numeric promotion**: integer operands narrower than int are first
promoted to int; the result type is then the smallest type that can
implicitly receive both operands:

- `int + int` → `int`; `byte + byte` → `int`
- `int + uint` → `long`; `long + uint` → `long`
- mixing `int`/`long` with `ulong` → compile error (unify the sign domain
  explicitly first)
- `int + float` / `float + int` → `float` (symmetric); any `double`
  operand → `double`

So `1 + 2.5 == 2.5 + 1 == 3.5` (symmetric). The result type is the
promoted type; assigning back to a narrower type requires an explicit
`as` (constant fit excepted). bool and char take no part in arithmetic.
The complete promotion and conversion rules are on
[Type Semantics](type-semantics.md).

`string + string` (OP_Add only) is concatenation; `"x" + 'y'` (char
implicitly converts to string) and `+` with a scalar operand concatenate
the same way. `string - string` and the other arithmetic forms are
compile errors. See [String](string.md).

### Shift

```nlang
a << b    a >> b
```

Bitwise left and right shift. **The left operand decides the result
type** (the Java/C# rule): integer left operands narrower than int are
promoted unarily to int (`byte << 2` is an `int`). The count (right
operand) follows the ordinary implicit-conversion rule into the result
type (the [Type Semantics](type-semantics.md) table): an int count works
on int and long carriers but not on uint or ulong, a long count only on
long, and a constant count that fits the carrier works on any of them.
Anything else is a compile error naming both the carrier and the count's
type (`1 << longCount`). enum
members ride their int32 carrier
(`Color.Green << 1 == 2`). float, double, bool, char, and string
operands are compile errors (`operator '<<' requires integer operands`);
a null operand reports `null is not a valid shift operand`.

`>>` on a signed row is an arithmetic shift (sign-extending,
`-8 >> 1 == -4`); on an unsigned row it is logical (zero-fill). `<<`
wraps in two's complement (`1 << 31` on int is `INT_MIN`). The count
must lie in `[0, bit width)` — a negative count or one that reaches the
type's width throws a runtime error (the division-by-zero family),
neither C undefined behavior nor Java mask wrapping.

Precedence sits between relational and additive: addition binds tighter
than shift (`1 << 2 + 3` is `1 << 5`), shift tighter than relational
(`1 << 2 < 8` compares `4 < 8`).

**Compound forms** `<<=` / `>>=` store back into the left value with
its declared type as the carrier — no unilateral promotion to int
(`byte b <<= 2` stays byte, so `1 <<= 7` wraps to -128 in the signed
byte row) — and store implicitly like the `+=` family does. The count
follows the binary form's admission rules (ordinary implicit conversion
into the carrier, constant rescue, the `[0, bit width)` runtime check),
so `b <<= 200` is a compile error and a negative variable count throws
the named runtime error. Supported left values are the
compound-assignment family (locals, fields, subscript elements); see
[Compound Assignment](compound-assignment.md).

Inside a generic argument list a `>>` still closes the nested type
(`List<List<int>>`); only at expression depth does it act as the shift
operator, so the two never conflict.

### Comparison

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

Comparison results are always **`bool`** (`true` / `false`).

**Comparison operands are typed**:

- **Mixing string with non-string is a compile error** (`"a" < 5`,
  `5 == "a"`). The only exception is the null literal: `s == null` /
  `c == null` compares against the null sentinel — identity for
  class/reference operands; for string operands, the null side reads as
  the empty string (handle 0 is the reserved null sentinel; a genuine
  empty string has its own object and is bitwise different from null).
  Hence `"" == null` compares equal; any non-empty string does not.
- **Numeric pairs** get the same symmetric promotion as arithmetic:
  `-2 < -1.5` promotes to double and is true; `1 == 1.0` is true. See
  [Primitives](primitives.md).
- **char compares only with char** (`'a' < 'b'` is legal); comparing a
  char against a number goes through no implicit conversion — take the
  code point first with `c as int` (`a char value can only be compared
  with a char value`).
- **bool compares only with bool, and only for equality**: `true == true`
  is true, but ordering a bool is a compile error (`bool values cannot be
  ordered; only == and != are supported`), and so is comparing a bool
  with any other type (`a bool value can only be compared with a bool
  value`).
- **String equality** compares content; string relational order is a
  C `strcmp`-style byte-wise comparison (e.g. `"Z" < "a"` is true because
  `'Z'`(90) < `'a'`(97)). Since strings are stored in the Unicode Transformation Format (UTF-8) and UTF-8 byte order
  equals code-point order, the ordering is also correct for non-American Standard Code for Information Interchange (ASCII)
  text: `"é" > "z"` is true. See [String](string.md) "Comparison".
- **Class/reference equality** (`==`, `!=`) is identity (same heap
  object). For value comparison, use the `equals` method and override it in
  your own classes — `==` is unaffected by the override; see [Object &
  Boxing](object.md) "`equals` and `getHashCode`" and [Class](class.md)
  "Virtual methods and overrides".
- **Arithmetic/concatenation with a null operand** is a compile error —
  null only has a value through the comparison-identity path above.

### Logical

```nlang
a && b   a || b   !a
```

Short-circuiting, C-style: `&&` evaluates `b` only when `a` is `true`;
`||` evaluates `b` only when `a` is `false`. The skipped operand has no
observable effect at all — no calls, no throws. The result is always
**`bool`** (never an original operand value). The operands of `&&`,
`||`, and `!` must be `bool` (`operator '&&' requires bool operands`) —
comparisons and predicates already produce bool. See
[Statements](statements.md) "Condition typing".
