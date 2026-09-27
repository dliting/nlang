# Operators


### Arithmetic

```nlang
a + b    a - b    a * b    a / b    a % b
```

Integer division truncates toward zero. Division/modulo by zero throws a
runtime error — this includes `float` division by zero, which throws rather
than producing IEEE 754 ±inf/NaN (NLang diverges from C/C++/Java here).

**Integer overflow** wraps silently in two's complement (C-style):
`INT_MAX + 1 == INT_MIN`. There is no SafeInt-style checking. Lock-in test:
`tests/e2e/int_overflow_wrap.n`.

**Numeric promotion**: arithmetic ops follow symmetric C-style promotion —
both operands are promoted to the wider type before the op:

- `int + int` → `int`
- `int + float` / `float + int` → `float` (both operands promoted to float)
- `float + float` → `float`

So `1 + 2.5 == 2.5 + 1 == 3.5` (symmetric). The result type is the promoted
type; assignment to a narrower type (e.g. `int r = 1.5 + 1;`) implicitly
truncates.

`string + string` (OP_Add only) is concatenation. `string - string` etc. are
compile errors. See [String](string.md).

### Comparison

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

Returns 1 (true) or 0 (false).

**Comparison operands are typed**:

- **Mixed string/non-string is a compile error** (`"a" < 5`, `5 == "a"`).
  The one exception is the null literal: `s == null` / `c == null` compare
  against the null sentinel — identity for class/reference operands; for a
  string operand the null side reads as the empty string (handle 0 is the
  reserved null sentinel; a real empty string has its own object, distinct
  from null by bits). `"" == null` therefore compares equal, and any non-empty
  string compares unequal.
- **int/float pairs** get the same symmetric promotion as arithmetic:
  `-2 < -1.5` promotes to float and is true; `1 == 1.0` is true. See
  [Primitives](primitives.md).
- **String equality** compares content; string relational ordering uses C
  `strcmp`-style byte-by-byte comparison (e.g. `"Z" < "a"` is true because
  `'Z'` (90) < `'a'` (97)). Because strings are UTF-8 and UTF-8 byte order
  equals code-point order, ordering is also correct for non-ASCII text:
  `"é" > "z"` is true. See [String](string.md) "Comparison".
- **Class/reference equality** (`==`, `!=`) is identity (same heap object).
  See [Class](class.md).
- **Arithmetic/concat with a null operand** is a compile error — null has a
  value only through the comparison identity path above.

### Logical

```nlang
a && b   a || b   !a
```

Short-circuit, C-style: `&&` evaluates `b` only when `a` is non-zero; `||`
evaluates `b` only when `a` is zero. The skipped operand produces no
observable effect at all — no calls, no throws. The result is always `int`
`0` or `1` (never the raw operand value). Operands of `&&`, `||` and `!` must
be `int` — the same rule as `if`/`while` conditions (comparisons already
produce `int`). See [Statements](statements.md) "Condition typing".
