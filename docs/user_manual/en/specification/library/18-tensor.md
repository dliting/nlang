# 18. The tensor Package

The `tensor` package provides numeric kernels over arrays of the
four scalar element kinds `int`, `long`, `float` and `double`.
Tensor values are ordinary arrays (§4.10): the package introduces
no new type, elements are read and written with the subscripts
of §5, and the array members of §4.10 apply unchanged. §18.1
fixes the package form; §18.2 defines construction and shape
operations; §18.3 defines the elementwise kernels and
reductions; §18.4 defines the matrix product; §18.5 defines the
example layer; §18.6 fixes the error model; §18.7 fixes the
device backends.

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
- `arange(n)` is the index spelling of the progression: `0`
  through `n - 1` in steps of one, `int` elements — the form
  that feeds `gather` and the subscript surfaces.
- `arange(start, stop, step)` returns the half-open arithmetic
  progression `start`, `start + step`, ... strictly before
  `stop`; the element type is the common type of the three
  arguments. `arange(0.0, 1.0, 0.25)` yields
  `0.0, 0.25, 0.5, 0.75`.
- A zero-filled array needs no kernel: sized creation `T[n]`
  is zero-initialized (§4.10); an all-ones array is
  `full(n, 1)`.
- `uniform(dims, low, high)` mints the dims-shaped array (the
  `full` creation rules: `dims` is a rank-1 `int[]`, the element
  count rides the §4.10 creation bound) and fills it row-major
  with `low + (high - low) * u`, one `u` per element drawn from
  the host pseudorandom number generator (PRNG) — the same
  seeded stream `math.random` reads, so
  a seeded run is reproducible through either surface. The
  element kind follows the bounds (`float`/`double`); the range
  is half-open, `[low, high)`.
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
  operand lengths everywhere else, which must be equal. A
  strided operand (a `transpose` result, a step slice) is read
  through its recorded strides on the device backend — the
  join makes no copy (§18.3); on the host it is materialized
  first (an invisible dense copy).
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
- `contiguous(a)` materializes `a` to a dense row-major
  array: when `a` is already row-major contiguous — an array
  the program created, or a view whose elements sit in
  dense order (a full-row slice, any offset) — `a` itself is
  returned and the result aliases it (a write through the
  result reaches `a`); otherwise a fresh dense copy is made
  and the result never aliases `a`. The copy keeps the
  source's residency (§18.7): a device view lands a device
  array. *Informative:* this is the explicit form of what the
  kernels do invisibly — feeding a strided view to any kernel
  needs no copy, and `contiguous` is for when the caller
  wants its own dense array to keep, index or hand out.

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
reads through the view's own shape and strides — the
reductions and `dot` take a strided view (a `transpose`
result, a step slice) with no copy, and on the device
backend so do `matmul` and `concat` (§18.7). On the host,
`matmul` and `concat` materialize a strided operand first —
an invisible dense copy the program never sees. A dense
copy as a value is an explicit decision: `tensor.contiguous`
(§18.2) materializes a strided view on demand.

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
`float` and `double` follow IEEE. A strided operand (a
`transpose` result, a row slice) is read through its recorded
strides on the device backend — the product makes no copy
(§18.3); on the host it is materialized first (an invisible
dense copy).

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
  The two-array form `softmax(x, axis)` reduces along an
  axis instead: for a rank-2 logit batch, `softmax(logits, 1)`
  softmaxes every row. The axis arm requires rank >= 2.
- `sigmoid(x)` is `1 / (1 + exp(-x))`, composed from the
  whole-array primitives.
- `tanh(x)` rides the vectorized tanh lane.
- `reluGrad(dOut, z)` is the ReLU gate backward: the gradient
  flows exactly where `z > 0` (`z = 0` gates off).
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

## 18.5.1 Array Arithmetic Operators

The arithmetic operators lower to the same kernels the
`tensor` package wraps — `a + b` on two numeric arrays is
`tensor.add(a, b)`, with the broadcasting rule of §18.3.1
and the same element-kind rules. A scalar on either side
rides the scalar arms (`2 * x` is `tensor.mulScalar(x, 2)`);
`s - a` and `s / a` lower to `tensor.rsubScalar` /
`tensor.rdivScalar` (the scalar sits on the left), `-a` to
`tensor.neg`. Unary minus, like every elementwise form here,
is declared for `int`, `long`, `float` and `double` only.
The operators require the package to be imported — they are
spellings of `tensor.*` calls, not a separate core surface.

```nlang
import tensor;

int main() {
    float[] a = tensor.arange(0.0f, 2.0f, 1.0f);   // 0 1
    float[] b = a * 2.0f - a;                   // [0, 1] again
    float[] c = 1.0f - a / 2.0f;
    a = a + b;
    float[,] m = float[2, 2];
    float[,] m2 = (m + m) * 2.0f;               // rank-2 broadcasts too
    return 0;
}
```

*Known limitations.* Compound assignment on arrays
(`a += b`) is not lowered — use the plain assignment with the
operator form. Element kinds `byte`/`ubyte` take no array
arithmetic. A rank-1 array with an axis arm
(`sum(v, 0)` on rank-1) is rejected; use the full-reduction
form. A floating-point subscript in an array store
(`grid[3.0, j]`) is not converted — index through an `int`.

## 18.5.2 Index and Comparison Primitives

The index surface removes the last per-element loops from
training and evaluation code — gathering each row's label
logit, locating a row's winner, counting the predictions that
match the truth.

- `gather(a, axis, indices)` takes one element per non-axis
  position: `indices` holds one entry per output element (its
  length equals the product of the dimensions other than
  `axis`), `indices[i]` names the axis position of the i-th
  non-axis position in row-major order, and the output drops
  the axis dim. For a rank-2 logit batch,
  `gather(logits, 1, labels)` reads `logits[i, labels[i]]`
  for every row — the label-gather spelling of a loss term.
  A rank-1 source degenerates to plain fancy indexing:
  removing the only axis leaves no positions to walk, so
  `gather(v, 0, indices)` returns `v[indices[i]]` with the
  length of `indices`. Views are ordinary operands —
  addressing rides the recorded strides.
- `argmax(a, axis)` and `argmin(a, axis)` return the int
  index array of the axis winners over the remaining dims,
  one index per non-axis position; ties keep the first along
  the scan.
- `eq(a, b)` compares two arrays of the same element kind
  elementwise (the broadcasting rule of §18.3.1 applies) and
  returns the int 0/1 array; floats follow IEEE `==` — NaN
  compares unequal to everything.
- `oneHot(labels, n)` maps a rank-1 int label array to the
  float `[len, n]` array with a one at each row's label — the
  target spelling of a classifier's training step.

### 18.5.3 Comparison Operators and Masks

On numeric-element arrays (`int`, `long`, `float`, `double`)
the six comparison operators are elementwise: `==`, `!=`, `>`,
`>=`, `<`, `<=` compare cell by cell (the broadcasting rule of
§18.3.1 applies, and either side may be a scalar) and produce
an `int` 0/1 mask — the same mask type `eq` produces, so
`tensor.sum(a > 0)` counts the positive cells and
`where` consumes the masks directly. Floats follow IEEE
ordering: NaN is false in every ordered comparison and in
`==`, true in `!=`. Arrays of non-numeric element kinds keep
the identity comparison for `==`/`!=` (two names for the same
array compare true), and `a == null` stays the null check —
neither form lowers to a mask.

```nlang
import tensor;

int main() {
    float[] f = tensor.arange(0.0f, 3.0f, 1.0f);
    int[] pos = f > 0.0f;              // [0, 1, 1] mask
    float[] relu = tensor.where(pos, f, 0.0f);
                                       // the relu spelling
    return 0;
}
```

### 18.5.4 Where and Scatter

- `where(mask, x, y)` selects per element: a nonzero mask
  cell picks `x`, a zero cell picks `y` — the relu idiom is
  `where(f > 0.0f, f, 0.0f)`. The mask must be an `int` or
  `long` array (the comparison operators' output); `x` and
  `y` are arrays of one shared element kind or scalars, and
  the three operands broadcast together (§18.3.1). The
  result's element kind is `x`/`y`'s kind.
- `scatter(a, axis, indices, values)` is the write twin of
  `gather`: per position it writes
  `out[position][indices[i]] = values[i]`, one value per
  index, and returns a NEW array (`a` is unchanged — value
  semantics, like every tensor function). `values` may be an
  array (its length rides the index count) or a scalar
  (broadcast to every written cell). An out-of-range index —
  negative or past the end, either bound — raises
  `IndexOutOfBoundsException`; indices do not wrap.

The language's fancy-index assignment lowers onto the same
write path in place: on a rank-1 numeric array,
`a[indices] = values` (values an array or a scalar) mutates
`a`'s own cells, so the mask idiom `a[a > 0] = 0` zeroes the
selected cells. The assignment raises the same
`IndexOutOfBoundsException` for an out-of-range index.

```nlang
import tensor;

int main() {
    int[] a = tensor.arange(0, 6, 1);
    int[] idx = int[2];
    idx[0] = 5; idx[1] = 0;
    int[] scattered = tensor.scatter(a, 0, idx, 42);
                                       // [42, 1, 2, 3, 4, 42]
    a[idx] = 42;                       // in place: same cells
    int[] zeroed = tensor.where(a > 4, a, 0);
                                       // mask select
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
and the rank. A `gather` index outside the axis range, a
`oneHot` label outside the class range, or a `scatter` (and a
fancy-index assignment) writing through an out-of-range index
raises `IndexOutOfBoundsException` naming the index and the
range; a `gather` index count that misses the non-axis position count
raises `Exception`. A `gather` whose gather axis is a zero
dimension raises `Exception`; a zero-size source whose gather
axis is nonzero gathers an empty result (its non-axis position
count is zero). A `uniform` call whose `low` exceeds its
`high` raises `Exception`.

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

## 18.7 Device Backends

The kernels can run on an accelerator. The language is
unchanged — the same programs, the same arrays, the same
results — and the device is a deployment detail, not a
program detail. This section fixes how the device is chosen,
which arrays live on it, and what a deployment ships.

### Selection

`nvm` names the backend with `--tensor-backend`
(§CLI, `nvm`):

```
nvm --tensor-backend cuda app.npkg
```

The same value can be set with the environment variable
`NLANG_TENSOR_BACKEND`; the flag overrides the variable. The
accepted values are `cpu`, `cuda`, `auto`, and `mirror`.

The default is `auto`. `auto` probes the installed device
backends and uses the first one whose driver comes up; if none
is present it falls back to `cpu` without a message. A shipped
build therefore runs everywhere: on a machine with a GPU the
kernels accelerate, on a machine without one they run on the
host, and the program is unchanged. Selecting `cuda` when the
CUDA plugin is absent is an error, not a fallback — naming a
backend you expect and silently running on the host would be a
lie. The same rule applies to a plugin that is present but
fails to come up: `auto` reports one line on the error stream
and continues on `cpu`, because a broken installation is worth
one report. `mirror` is a host-side test backend: it exercises
every device path over ordinary host memory, so the device code
can be accepted on a machine with no GPU. `cpu` always runs the
host path.

`tensor.backend()` reports the active backend by name
(`"cpu"`, `"cuda"`, or `"mirror"`), so a program can state its
own device choice.

### Where arrays live

When a device backend is active, the four numeric element
kinds (`int`, `long`, `float`, `double`) created by the
language follow the device (§4.10.2): `float[3]`, `Array(dims)`,
and the tensor creation family all mint device-resident arrays.
`byte` and `ubyte` arrays never leave the host — they are the
I/O buffer domain (file and network payloads), and a device
pointer there would be a foreign dereference.

Arrays move on request, not on accident. `tensor.toDevice(a)`
lands a copy on the active device and `tensor.toHost(a)` lands
one back; an operand already on the target is returned as-is, so
both are safe to call unconditionally. A view's `toDevice`
materializes the view's elements into the device copy. A kernel
whose operands disagree on residency raises `Exception`, naming
both sides and the fix:

```
tensor: operands on different devices (cuda vs host);
        move them with tensor.toDevice/toHost
```

### Reading and writing a device array

Subscripting a device array reads and writes one element per
access, transferring that element across the device boundary
each time. This is the same semantics as `tensor[0]` on a
PyTorch or CuPy array — a transparent element access, not a
copy of the whole array. Printing or serializing a device array
transfers the payload once, in a single block, not element by
element.

*Informative:* a hot loop that indexes a device array one
element at a time pays a transfer per index. Express the loop as
a tensor kernel instead (`tensor.add`, `tensor.sum`, …) and the
data moves once. Element-level subscripts are for the
interactive and I/O edges of a program, not for the inner loop.

### Execution model

Device work is asynchronous: the tensor operations enqueue
kernels on the device and return without waiting for them to
finish, so a chain of device operations pipelines instead of
paying a wait per operation. The results are guaranteed complete
at every point where the program can observe them — reading or
writing an element, printing or serializing, `tensor.toHost` and
`tensor.toDevice`, and the scalar reductions (`tensor.sum`,
`tensor.dot`, `tensor.max`, `tensor.min`, `tensor.mean`) — and
each of these waits for the device work enqueued so far. A
program cannot observe a half-finished device array.

The device errors the language raises — an integer division by
zero, a negative `gather` index, an out-of-range `oneHot` label —
are raised at the operation itself, exactly as on the host.
Faults detected while the device runs its kernels (an illegal
access, for example) surface at the next observation point rather
than at the operation that caused them; see the known limitations
below.

### Deployment

The CUDA backend is a plugin: `nlang_tensor_cuda.dll` beside
`nlang_tensor.dll` in the native directory. Copy the two files
together and the GPU path is available; omit the plugin and
`auto` falls back to `cpu` with no further configuration. The
host runtime never links the GPU driver, so a build without the
plugin runs identically on a machine without a GPU.

The backend contract is device-agnostic: a future accelerator
(ROCm/HIP, for example) plugs in by shipping a sibling plugin
DLL against the same contract, with no change to the core.

### Cross-backend agreement

The device path raises the same exception classes with the
same message text as the host path — a `DivByZeroException` in
an integer division, an `IndexOutOfBoundsException` for a
gather index — so a `catch` written for the host reads the same
on the device. Floating-point results may differ in the last
digit between backends (the host and device use different
reduction orderings); a program that asserts exact equality
across backends is asserting on an implementation detail.

### Known limitations

Two operations are not yet on the CUDA backend, and the device
concat and per-axis reductions cover only part of the surface.

* `tensor.clamp` on an integer array (`int`, `long`) raises
  `Exception` — only the `float` and `double` clamp are on the
  device.
* `tensor.gather` with `double` or `long` elements raises
  `Exception` — the `float` and `int` gather are on the device.
* `tensor.concat` along any axis but the outermost (axis 0) raises
  `Exception` — the device kernel implements the axis-0 join
  only; the host path handles every axis.
* `sum`/`max`/`min` with an axis arm (`reduce`) on the device
  supports `float` only, and only the last axis (any rank) or axis 0
  of a rank-2 array; other kinds and middle axes of a rank-3+ array
  raise `Exception` — the host path handles every kind and axis.
* `argmax`/`argmin` with an axis arm on the device supports `float`
  only and only the last axis; other kinds and axes raise
  `Exception` — the host path handles every kind and axis.
* A fault detected while the device runs its kernels (an illegal
  access, for example) is reported at the next observation point,
  not at the operation that caused it — deferred attribution, the
  same tradeoff the mainstream device stacks make. In the narrow
  case where such a fault is followed only by output (printing)
  with no further tensor operation, the output can be wrong
  instead of raising.
