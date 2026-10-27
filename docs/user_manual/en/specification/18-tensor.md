# 18. The tensor Package

The `tensor` package provides numeric kernels over arrays of the
four scalar element kinds `int`, `long`, `float` and `double`.
Tensor values are ordinary arrays (§4.3): the package introduces
no new type, elements are read and written with the subscripts
of §5, and the array members of §4.3 apply unchanged. §18.1
fixes the package form; §18.2 defines construction and shape
operations; §18.3 defines the elementwise kernels and
reductions; §18.4 defines the matrix product; §18.5 fixes the
error model.

## 18.1 Package Form

`tensor` is a standard library package (§12.4): `import tensor;`
brings it into scope and every function is called through the
package qualifier (`tensor.add(a, b)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; the kernels are `native` functions
(§7.5) implemented by the native library shipped with the
toolchain. Every kernel is a blocking native function: it
declares a blocking section (§11.1), so the calling context
parks and the carrier lock is released while the kernel runs.
Argument admission is the package rule of §15.1.

Every family uses same-name overloads: the compiler selects the
declaration by argument type, so the element kind comes from
the typed arguments — the fill value for `full`, the
progression triple for `arange`, the array operands everywhere
else. An exact argument match selects over a wider one:
`arange(0, 10, -3)` selects the `int` overload. The suffixed
per-kind declarations (`fullI`, `arangeD`, and their family)
are internal implementation of the overloads, not part of the
documented surface.

## 18.2 Construction and Shapes

- `full(n, value)` returns an array of `n` copies of `value`;
  the element type is the type of `value`.
- `arange(start, stop, step)` returns the half-open arithmetic
  progression `start`, `start + step`, ... strictly before
  `stop`; the element type is the common type of the three
  arguments.
- A zero-filled array needs no kernel: sized creation `T[n]`
  is zero-initialized (§4.3).
- `reshape1`–`reshape8` return the flat source array `a`
  reshaped to the rank carried by the name, filled in row-major
  order. The `int[]` `shape` must have exactly that many
  dimensions and their product must equal the element count of
  `a`. The declared surface covers target ranks 1–8; the global
  rank bound of §4.3 is 64.
- `flatten(a)` returns the rank-2 array `a` as a rank-1 array
  in row-major order.
- `transpose(a)` returns the rank-2 array `a` as `[cols, rows]`
  in row-major order.

## 18.3 Elementwise Kernels and Reductions

- `add(a, b)`, `sub(a, b)`, `mul(a, b)` and `div(a, b)` apply
  the operator element by element and return an array of the
  operand element type. The operands must have equal lengths
  (rank 1) or equal shapes (rank 2). Integer `div` by zero
  raises `DivByZeroException` (§9.3); `float` and `double`
  division follows IEEE, yielding Inf or NaN (§4.2).
- `sum(a)` returns the sum of all elements as the element type;
  the empty array yields the type's zero.
- `max(a)` and `min(a)` return the largest and smallest element
  as the element type; the empty array raises `Exception`
  (§9.3).
- `mean(a)` returns the arithmetic mean as a `double` for every
  element type; the empty array raises `Exception`.
- `dot(a, b)` returns the inner product of two equal-length
  arrays. The `int` and `long` overloads accumulate in 64 bits
  and return `long`; the `float` and `double` overloads return
  their own type. The empty array yields zero.

For rank-2 operands the elementwise kernels keep the operand
shape, and the reductions flatten over the elements in
row-major order.

## 18.4 Matrix Kernels

`matmul(a, b)` returns the matrix product of two rank-2 arrays:
`[aRows, aCols] * [bRows, bCols]` yields `[aRows, bCols]`, and
the inner dimensions must match. The arithmetic follows the
language-wide rules (§4.2): `int` and `long` wrap around,
`float` and `double` follow IEEE.

## 18.5 Error Behavior

Every tensor failure is a catchable value of the `Exception`
family (§9.3). A `null` array argument raises
`NullPointerException`. A negative creation length, or an
element count past the creation bound of §4.3, raises
`IndexOutOfBoundsException` at the creation site; a negative
`reshape` dimension raises the same class. `arange` checks its
element count before minting and raises `Exception` when the
progression would exceed the bound. A zero `arange` step, or one that
cannot advance toward `stop`, raises `Exception`. A length or
shape mismatch in a two-operand kernel, a `reshape` shape whose
length misses the target rank or whose product misses the
element count, and `max`, `min` or `mean` on an empty array all
raise `Exception`, naming both shapes or both counts.

> Implementation note. Kernels read and write the array
> payloads through pinned views, so one kernel call replaces an
> entire interpreter loop.
