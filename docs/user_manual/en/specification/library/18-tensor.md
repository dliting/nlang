# 18. The tensor Package

The `tensor` package provides numeric kernels over arrays of the
four scalar element kinds `int`, `long`, `float` and `double`.
Tensor values are ordinary arrays (§4.10): the package introduces
no new type, elements are read and written with the subscripts
of §5, and the array members of §4.10 apply unchanged. §18.1
fixes the package form; §18.2 defines construction and shape
operations; §18.3 defines the elementwise kernels and
reductions; §18.4 defines the matrix product; §18.5 defines the
example layer; §18.6 fixes the error model.

## 18.1 Package Form

`tensor` is a standard library package (§3.9.4): `import tensor;`
brings it into scope and every function is called through the
package qualifier (`tensor.add(a, b)`). The package name is
single-segment (§3.3). Its declarations are the standard
library's `.n` source; the kernels are `native` functions
(§7.5) implemented by the native library shipped with the
toolchain. Every kernel is a blocking native function: it
declares a blocking section (§10.1), so the calling context
parks and the carrier lock is released while the kernel runs.
Argument admission is the package rule of §14.1.

Every family uses same-name overloads: the compiler selects the
declaration by argument type, so the element kind comes from
the typed arguments — the fill value for `full`, the
progression triple for `arange`, the array operands everywhere
else. An exact argument match selects over a wider one:
`arange(0, 10, -3)` selects the `int` overload, and `long`
arguments select the `long` one. The suffixed per-kind
declarations (`fullI`, `arangeD`, and their family) are
internal implementation of the overloads, not part of the
documented surface.

## 18.2 Construction and Shapes

- `full(n, value)` returns a rank-1 array of `n` copies of
  `value`; `full(dims, value)` fills the shape described by the
  rank-1 `int[]` `dims` instead. The element type is the type of
  `value`; every family member is selected by that type.
- `arange(start, stop, step)` returns the half-open arithmetic
  progression `start`, `start + step`, ... strictly before
  `stop`; the element type is the common type of the three
  arguments. `arange(0.0, 1.0, 0.25)` yields
  `0.0, 0.25, 0.5, 0.75`.
- A zero-filled array needs no kernel: sized creation `T[n]`
  is zero-initialized (§4.10); an all-ones array is
  `full(n, 1)`.
- `reshape(a, shape)` returns the elements of `a` in the shape
  described by the rank-1 `int[]` `shape`, read and filled in
  row-major order; the product of the dimensions must equal the
  element count of `a`, and the result rank follows the global
  bound of §4.10 (a bracket-annotated result gets its rank
  checked against the annotation). One dimension of `shape` may
  be `-1`: it is inferred by dividing the element count by the
  product of the remaining dimensions, and the division must
  come out exact. At most one `-1` is allowed. When the elements
  already sit in row-major order without gaps — an array the
  program created, or a view that selects them contiguously —
  the result is a view sharing those elements; otherwise a fresh
  copy is made and the result never aliases `a`.
- `concat(a, b, axis)` joins two arrays of the same rank along
  `axis`: the result takes the summed length on `axis` and the
  operand lengths everywhere else, which must be equal.
- `flatten(a)` returns the elements of an array of any rank as a
  rank-1 array in row-major order — a view when the elements are
  contiguous, a copy otherwise (the same rule as `reshape` with
  one dimension). *Informative:* this matches NumPy's `ravel`,
  not NumPy's `flatten`, which always copies; NLang's name keeps
  the one-word spelling and the view-when-possible semantics.
- `transpose(a)` reverses the axes of an array of any rank, and
  `transpose(a, axes)` permutes them: `axes` is a rank-1 `int[]`
  naming each source axis once, in the order the result reads
  it. Both return views aliasing `a` — a transposed element
  write reaches the source (§4.10.9).
- `slice(a, starts, stops, steps)` is the function form of the
  slice subscript (§4.10.9): three rank-1 `int[]` arrays, one
  entry per axis, with the same normalization and the same view
  result.
- `broadcastTo(a, shape)` returns a view of `a` with the shape
  described by the rank-1 `int[]` `shape`: each target
  dimension must equal the source's or the source's must be 1
  (the size-1 axis repeats), and leading dimensions may be
  added. The repeated axes address one source cell each; a
  store through a repeated axis that spans more than one
  position raises `Exception` (§4.10.9), while size-1 repeated
  axes stay writable. Element reads, reductions and
  serialization see the repeated content.

```nlang
import io;
import tensor;

int main() {
    int[] shape = int[2];
    shape[0] = 2; shape[1] = 3;
    double[,] m = tensor.full(shape, 2.5);   // 2x3, every element 2.5
    int[] six = int[1];
    six[0] = 6;
    double[] flat = tensor.reshape(m, six);  // one dimension -> rank 1
    io.print(flat.length());                 // 6
    int[] inferred = int[2];
    inferred[0] = -1; inferred[1] = 2;
    double[,] rows = tensor.reshape(m, inferred);  // [-1, 2] -> [3, 2]
    io.print(rows.length());                 // 6
    return 0;
}
```

## 18.3 Elementwise Kernels and Reductions

- `add(a, b)`, `sub(a, b)`, `mul(a, b)` and `div(a, b)` apply
  the operator element by element and return an array of the
  operand element type. The two arrays broadcast (§18.3.1); a
  scalar second argument (`add(a, 0.5)`) applies to every
  element. Integer `div` by zero raises `DivByZeroException`
  (§8.3); `float` and `double` division follows IEEE, yielding
  Inf or NaN (§4.2).
- `exp(a)` and `log(a)` apply the mathematical function
  element by element (`e` raised to the element, natural
  logarithm) and return an array of the operand element type.
  They are declared for `float` and `double` only.
- `max(a, bound)` and `min(a, bound)` return a copy of `a`
  with every element clipped at `bound` (raised to the bound
  from below, lowered from above); the element type is the
  common type of the element and the bound.
- `sum(a)` returns the sum of all elements as the element type;
  the empty array yields the type's zero. `sum(a, axis)` sums
  along one axis and drops it from the shape: a rank-2 `(3, 4)`
  summed on axis 0 yields `(4,)`, and the last axis of a rank-1
  array yields the scalar.
- `max(a)` and `min(a)` return the largest and smallest element
  as the element type; the empty array raises `Exception`
  (§8.3).
- `mean(a)` returns the arithmetic mean as a `double` for every
  element type; the empty array raises `Exception`.
- `dot(a, b)` returns the inner product of two equal-length
  arrays. The `int` and `long` overloads accumulate in 64 bits
  and return `long`; the `float` and `double` overloads return
  their own type. The empty array yields zero.

For rank-2 operands the elementwise kernels keep the operand
shape, and the reductions flatten over the elements in
row-major order. Views are ordinary operands: every kernel
reads through the view's own shape and strides (a `transpose`
result feeds `matmul` directly), and kernels that need
contiguous cells — `matmul` and `concat` — copy a
non-contiguous operand first, invisibly to the program.

```nlang
import io;
import tensor;

int main() {
    double[] a = tensor.arange(0.0, 1.0, 0.25);    // 0.0 0.25 0.5 0.75
    double[] b = tensor.full(4, 2.0);
    double[] c = tensor.add(a, b);                 // 2.0 2.25 2.5 2.75
    io.print(tensor.sum(c));                       // 9.5
    io.print(tensor.dot(a, a));                    // 0.875
    double[] e = tensor.exp(c);                    // element-wise e^x
    double[] lo = tensor.max(a, 0.5);              // 0.5 0.5 0.5 0.75
    return 0;
}
```

### 18.3.1 Broadcasting

The two-array kernels pair elements by the right-aligned
broadcast rule, the same rule NumPy uses: ranks may differ —
the operand with fewer dimensions is padded with leading axes
of length 1 — and on each aligned axis the two lengths must be
equal, or one of them 1; the result takes the larger length on
every axis, and a size-1 operand axis repeats its single value.

```nlang
double[,] m = tensor.full([3, 4], 0.0);   // see §18.2 for dims
double[] bias = tensor.full(4, 1.0);
double[,] biased = tensor.add(m, bias);   // (3,4)+(4,): rows repeat
```

An axis pair that is neither equal nor 1 raises `Exception`
with both shapes (§18.6).

## 18.4 Matrix Kernels

`matmul(a, b)` returns the matrix product of two rank-2 arrays:
`[aRows, aCols] * [bRows, bCols]` yields `[aRows, bCols]`, and
the inner dimensions must match. The arithmetic follows the
language-wide rules (§4.2): `int` and `long` wrap around,
`float` and `double` follow IEEE.

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,] t = tensor.transpose(m);      // [[1, 3], [2, 4]]
int[] f = tensor.flatten(m);         // [1, 2, 3, 4]
int[,] p = tensor.matmul(m, m);      // [[7, 10], [15, 22]]
```

## 18.5 Example Layer

Three functions of the package are written in NLang itself —
their source is the package's `.n` file, readable and
modifiable like any library function (§3.9.4). They compose the
native kernels instead of adding new ones:

- `relu(a)` clips the array at zero from below
  (`max(a, 0)`), declared for `float` and `double` — an integer
  element has no activation semantics.
- `softmax(x)` exponentiates the shifted elements
  (`x - max(x)`, which keeps large inputs from overflowing
  `exp`) and normalizes by their sum; the result totals 1.
- `crossEntropyLoss(logits, labels)` computes the cross-entropy
  of one-hot `labels` against `softmax(logits)`:
  `-sum(labels * log(softmax(logits)))`. The formals are
  rank-1: a rank-2 batch flattens first (`flatten`, §18.2).

```nlang
import io;
import tensor;

int main() {
    double[] logits = [2.0, 1.0, 0.1];
    double[] labels = [1.0, 0.0, 0.0];
    double loss = tensor.crossEntropyLoss(logits, labels);
    io.print(loss);                    // ≈ 0.417 (soft target of argmax)
    double[] shifted = tensor.sub(logits, [1.0, 1.0, 1.0]);  // [1, 0, -0.9]
    double[] act = tensor.relu(shifted);                     // [1, 0, 0]
    io.print(act[2]);                  // 0 — clipped at zero
    return 0;
}
```

## 18.6 Error Behavior

Every tensor failure is a catchable value of the `Exception`
family (§8.3). A `null` array argument raises
`NullPointerException`. A negative creation length, or an
element count past the creation bound of §4.10, raises
`IndexOutOfBoundsException` at the creation site; a negative
`reshape` dimension (other than the single inferred `-1`)
raises the same class. `arange` checks its
element count before minting and raises `Exception` when the
progression would exceed the bound. A zero `arange` step, or one that
cannot advance toward `stop`, raises `Exception`. A broadcast
mismatch in a two-operand kernel, a `reshape` shape whose
length misses the target rank, whose product misses the
element count, or whose `-1` inference divides unevenly, a
`transpose` permutation that misses or repeats an axis, a
`broadcastTo` target a source shape cannot fill, a `sum` axis
outside the array's rank, a `slice` list whose length misses
the rank, a `concat` whose operands disagree on rank or on the
non-axis dimensions, and `max`, `min` or `mean` on an empty
array all raise `Exception`; the shape and length mismatch
messages name both sides, and the axis messages name the axis
and the rank.

*Informative:* the diagnostics read like
`tensor.matmul: shape mismatch (2x3 vs 2x2)` for `matmul` and
`tensor: shape mismatch (2x3 vs 2x2)` for the elementwise
kernels.

> Implementation note. The kernels run as native code in
> `nlang_tensor.dll`, reading and writing the array payloads
> through pinned views, so one kernel call replaces an entire
> interpreter loop. *Informative:* measurements on the shipped
> build put elementwise `add` at roughly 49× the hand-written
> loop and `dot` at roughly 117×; the figures describe the
> current implementation, not a promise of the language.
