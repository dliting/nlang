# Primitives


`int` and `float` are the two numeric primitive value types. A third
primitive, `string`, has its own page: [String](string.md).

| Type  | Size    | Description            |
|-------|---------|------------------------|
| `int`   | 4 bytes | 32-bit signed integer  |
| `float` | 4 bytes | 32-bit IEEE 754 float  |

### Numeric literals

Integer literals may use exponent notation — `2e5` is `200000`. The value
must be integral and fit the 32-bit integer range; `1e30` (out of range) and
`2e-1` (= 0.2, fractional) are compile errors. Float literals require a
decimal point and may use exponents (`1.0e30`, `2.5e-3`); a bare `1e30` is an
`int` literal, not a float.

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
truncates. `string + string` is concatenation (see [String](string.md));
`string - string` and the other arithmetic forms are compile errors.

### Comparison

```nlang
a == b   a != b   a < b   a > b   a <= b   a >= b
```

`int`/`float` pairs get the same symmetric promotion as arithmetic:
`-2 < -1.5` promotes to float and is true; `1 == 1.0` is true. The full
comparison rules across all types (string, class, null) are on
[Operators](operators.md) "Comparison".

### Casting

`int` and `float` convert to each other with an explicit cast — see
[Type Casts](type-casts.md).
