# Operators


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
- **String equality** compares content; string relational order is a
  C `strcmp`-style byte-wise comparison (e.g. `"Z" < "a"` is true because
  `'Z'`(90) < `'a'`(97)). Since strings are Unicode Transformation Format (UTF-8) and UTF-8 byte order
  equals code-point order, the ordering is also correct for non-American Standard Code for Information Interchange (ASCII)
  text: `"é" > "z"` is true. See [String](string.md) "Comparison".
- **Class/reference equality** (`==`, `!=`) is identity (same heap
  object). See [Class](class.md).
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
[Statements](statements.md) "Condition types".
