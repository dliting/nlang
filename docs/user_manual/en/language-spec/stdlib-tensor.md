# tensor — numeric array kernels

The `tensor` package provides numeric kernels over the four scalar
element kinds (`int`, `long`, `float`, `double`). Kernels run as native
code in `nlang_tensor.dll` — the array payloads are read and written
directly through pinned views, so a kernel call replaces an entire
interpreter loop (stage-1 measurements: elementwise add ≈ 49× the
hand-written loop, dot ≈ 117×).

**One naming discipline**: every family uses same-name overloads — the
compiler picks the declaration by argument type, so the element kind
comes from the typed arguments (`full(n, value)` takes it from `value`,
`arange(start, stop, step)` from the progression triple, the array
families from the array operands). Exact argument matches beat wider
ones: `arange(0, 10, -3)` yields `int[]`; pass `long` arguments for a
`long[]`. Zero-filled arrays need no kernel — the language's sized
creation `T[n]` is already zero-initialized (`double[256]`), and "ones"
is `full(n, 1)`. The tables below are the package's whole documented
surface; suffixed declarations in `stdlib/tensor.n` (`fullI`,
`arangeD`, ...) are internal implementation of the overloads, not part
of it.

| Function | Signature | Notes |
|----------|-----------|-------|
| full | (int n, T value) → T[] | n copies of value |
| arange | (T start, T stop, T step) → T[] | half-open progression; see below |
| add sub mul div | (T[], T[]) → T[] | elementwise binary; same-name overloads |
| sum | (T[]) → T | empty array → 0 |
| max min | (T[]) → T | empty array → Exception |
| mean | (T[]) → double | always double; empty array → Exception |
| dot | (T[], T[]) → T | int/long overloads return long (64-bit accumulate) |

**arange** follows the half-open rule: elements are `start`,
`start+step`, ... strictly before `stop` — `arange(0.0, 1.0, 0.25)`
yields `0.0, 0.25, 0.5, 0.75`. A zero step, or one that cannot advance
toward `stop`, raises the base Exception.

**Error semantics** mirror the language built-ins: a null array raises
`NullPointerException`, a negative creation length or one past the 100M-element cap raises
`IndexOutOfBoundsException`, integer `div` by zero raises
`DivByZeroException` (float/double division follows IEEE — Inf/NaN),
length mismatches raise the base Exception, and the empty-array rules
are per the table above. All of them are catchable `Exception` family
values.

## Matrix and rank-2 kernels

The rank-2 face mirrors the 1-D names on `T[,]` operands, and the
matrix family adds shape-changing and product kernels:

| Function | Signature | Notes |
|----------|-----------|-------|
| add sub mul div | (T[,], T[,]) → T[,] | elementwise binary; shapes must be equal |
| sum | (T[,]) → T | flattens in row-major order |
| max min | (T[,]) → T | flattens; empty matrix → Exception |
| mean | (T[,]) → double | always double; empty matrix → Exception |
| transpose | (T[,]) → T[,] | [r, c] becomes [c, r] |
| flatten | (T[,]) → T[] | row-major copy to rank 1 |
| reshape1..8 | (T[], int[] shape) → T[...] | target rank carried by the name; see below |
| matmul | (T[,], T[,]) → T[,] | matrix product; int/long wrap around, float/double follow IEEE |

**transpose** and **flatten** are block copies — `transpose` maps
`[r, c]` to `[c, r]`, `flatten` copies row-major into a rank-1 array.

**reshape1**–**reshape8** take a flat `T[]` source (call `flatten`
first to adapt a higher-rank matrix) and an `int[]` shape whose
length must equal the target rank carried by the name and whose
product must equal the element count; a negative dimension, a wrong
length, or a product mismatch raises the base Exception with both
numbers spelled out.

**matmul** requires two rank-2 operands with agreeing inner
dimensions (columns of `a` = rows of `b`); mismatches raise the base
Exception reporting both shapes (`tensor.matmul: shape mismatch
(2x3 vs 2x2)`). The elementwise rank-2 kernels require equal shapes
and report them the same way (`tensor: shape mismatch (2x3 vs 2x2)`).

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,] t = tensor.transpose(m);      // [[1, 3], [2, 4]]
int[] f = tensor.flatten(m);         // [1, 2, 3, 4]
int[,] p = tensor.matmul(m, m);      // [[7, 10], [15, 22]]
```

```nlang
import io;
import tensor;

int main() {
    double[] a = tensor.arange(0.0, 1.0, 0.25);    // 0.0 0.25 0.5 0.75
    double[] b = tensor.full(4, 2.0);
    double[] c = tensor.add(a, b);                 // 2.0 2.25 2.5 2.75
    io.print(tensor.sum(c));                       // 9.5
    io.print(tensor.dot(a, a));                    // 0.625
    return 0;
}
```
