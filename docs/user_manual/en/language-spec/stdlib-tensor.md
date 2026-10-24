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
is `full(n, 1)`.

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
