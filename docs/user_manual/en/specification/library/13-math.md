# 13. The math Package

## 13.1 Package Form

`math` is a standard library package (§3.9.4): `import math;`
brings it into scope and every function is called by its fully
qualified name (`math.sqrt(2)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; the implementations are `native`
functions (§7.5) carried by the host native library named from
the package.

## 13.2 Precision

Every floating-point function in the package runs at `double`
precision: parameters and results are `double` (§4.2). Integer
and `float` arguments enter through the implicit widening of
§4.4, so `math.sqrt(4)` and `math.sin(1.5f)` both compile. A
`double` result reaches a `float` target only through an
explicit `as` conversion (§5.5).

## 13.3 Functions

The package declares 26 functions.

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
| `random()` | () → `double` | the next generator value, in `[0, 1)` (§13.5) |
| `srand(seed)` | (`int seed`) → `void` | re-seeds the generator (§13.5) |
| `randomi(min, max)` | (`int min`, `int max`) → `int` | a value drawn from the closed range `[min, max]` (§13.5) |
| `randomBytes(count)` | (`int count`) → `byte[]` | `count` fresh bytes from the operating system's cryptographically secure random source, never from the generator of §13.5; raises `Exception` naming the cause — `math.randomBytes: count is negative.`, `math.randomBytes: count is too large.` (a count above the 16 MiB request ceiling) or `math.randomBytes: the system random source is unavailable.` (§13.5) |

## 13.4 Error Behavior

Failures split into two models.

- A domain error of the transcendental family — `sqrt` of a
  negative, `log` of a non-positive, `asin` or `acos` outside
  `[-1, 1]` — yields a NaN `double` (§4.2) and does not raise;
  the package follows the C convention. A NaN propagates
  through further arithmetic.
- An argument error raises `Exception` (§8.3): `absi` of the
  most negative `int`, `clampi` or `clampf` with `lo > hi`,
  `randomi` with `min > max`, `randomBytes` with a `count`
  below `0` or above `16777216`, and `floor`, `ceil` or `round`
  when `x` is NaN or the rounded value lies outside the `long`
  range. `randomBytes` raises the same base `Exception` when
  `count` is negative, when it exceeds the 16 MiB request
  ceiling, or when the operating system's random source cannot
  supply bytes.

## 13.5 Random Numbers

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
> program start; no `std::uniform_*_distribution` is used —
> those are implementation-defined and not portable.
> `randomBytes` is not drawn from this generator: it forwards
> to the operating system's cryptographic source.

`randomBytes` shares none of this. It draws from the operating
system's cryptographically secure pseudorandom number generator
(CSPRNG) rather than from `std::mt19937` — what unguessable
values like session ids and multipart boundaries are built from
in the web package. Its draws are not reproducible and `srand`
does not move them, so experiments that must replay take
`random`/`randomi`, and tokens that must be unguessable take
`randomBytes`.

The two scalings, in pseudocode:

```text
random()  = (double)((next() >> 8) * (1.0 / 16777216.0))   // 24-bit mantissa, exact in [0,1)
randomi(min,max) = min + (int32)(next() % (uint32)(max - min + 1))   // small modular bias, documented
```
