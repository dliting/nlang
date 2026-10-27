# 14. The math Package

## 14.1 Package Form

`math` is a standard library package (§12.4): `import math;`
brings it into scope and every function is called through the
package qualifier (`math.sqrt(2)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; the implementations are `native`
functions (§7.5) carried by the host native library named from
the package.

## 14.2 Precision

Every floating-point function in the package runs at `double`
precision: parameters and results are `double` (§4.2). Integer
and `float` arguments enter through the implicit widening of
§4.12, so `math.sqrt(4)` and `math.sin(1.5f)` both compile. A
`double` result reaches a `float` target only through an
explicit `as` conversion (§5.5).

## 14.3 Functions

The package declares 25 functions.

| Function | Signature | Effect |
| --- | --- | --- |
| `sqrt(x)` | (`double x`) → `double` | the non-negative square root of `x` |
| `sin(x)` / `cos(x)` / `tan(x)` | (`double x`) → `double` | the trigonometric ratio of the radian `x` |
| `asin(x)` / `acos(x)` | (`double x`) → `double` | the radian whose sine (`asin`) or cosine (`acos`) is `x` |
| `atan(x)` | (`double x`) → `double` | the radian whose tangent is `x` |
| `atan2(y, x)` | (`double y`, `double x`) → `double` | the radian of the direction from the origin to the point `(x, y)` |
| `pow(x, y)` | (`double x`, `double y`) → `double` | `x` raised to the power `y` |
| `exp(x)` | (`double x`) → `double` | `e` raised to the power `x` |
| `log(x)` | (`double x`) → `double` | the natural logarithm of `x` |
| `absi(x)` | (`int x`) → `int` | the absolute value of `x` |
| `absf(x)` | (`double x`) → `double` | the absolute value of `x` |
| `mini(a, b)` / `maxi(a, b)` | (`int a`, `int b`) → `int` | the smaller (`mini`) or larger (`maxi`) argument |
| `minf(a, b)` / `maxf(a, b)` | (`double a`, `double b`) → `double` | the smaller (`minf`) or larger (`maxf`) argument |
| `clampi(v, lo, hi)` | (`int v`, `int lo`, `int hi`) → `int` | `v` forced into the closed range `[lo, hi]` |
| `clampf(v, lo, hi)` | (`double v`, `double lo`, `double hi`) → `double` | `v` forced into the closed range `[lo, hi]` |
| `floor(x)` | (`double x`) → `long` | `x` rounded toward negative infinity |
| `ceil(x)` | (`double x`) → `long` | `x` rounded toward positive infinity |
| `round(x)` | (`double x`) → `long` | `x` rounded half away from zero |
| `random()` | () → `double` | the next generator value, in `[0, 1)` (§14.5) |
| `srand(seed)` | (`int seed`) → `void` | re-seeds the generator (§14.5) |
| `randomi(min, max)` | (`int min`, `int max`) → `int` | a value drawn from the closed range `[min, max]` (§14.5) |

## 14.4 Error Behavior

Failures split into two models.

- A domain error of the transcendental family — `sqrt` of a
  negative, `log` of a non-positive, `asin` or `acos` outside
  `[-1, 1]` — yields a NaN `double` (§4.2) and does not raise;
  the package follows the C convention. A NaN propagates
  through further arithmetic.
- An argument error raises `Exception` (§9.3): `absi` of the
  most negative `int`, `clampi` or `clampf` with `lo > hi`,
  `randomi` with `min > max`, and `floor`, `ceil` or `round`
  when `x` is NaN or the rounded value lies outside the `long`
  range.

## 14.5 Random Numbers

The package draws from one generator per program run.

- `random()` yields one of 2^24 distinct values, scaled exactly
  onto `[0, 1)`.
- `randomi(min, max)` draws from the closed range by modulo
  reduction; a range whose size does not divide 2^32 carries a
  small bias.
- After `srand(seed)`, the sequence is fixed by the seed and
  identical across platforms. The generator's state before any
  `srand` call is *unspecified* (§0.4).

> Implementation note. The shipped generator is the Mersenne
> Twister `std::mt19937`, seeded from `std::random_device` at
> program start; `random()` takes the high 24 bits of a draw
> and `randomi` reduces a draw modulo the range size.
