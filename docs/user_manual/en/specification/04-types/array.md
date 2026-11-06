# 4.10 Arrays

## 4.10.1 Element type and rank

An array type is fixed by its *element type* and — when written
with brackets — its *rank*: the number of dimensions, at least 1
and at most 64. Among bracket spellings the rank is part of the
type: `int[]` and `int[,,]` are different types, and so are
`int[]` and `long[]`. A rank above 64 is a compile-time error.

The rank is written as a single bracket suffix group after the
element type (§2.3): `[]` is rank 1, `[,]` is rank 2, `[,,]` is
rank 3, and so on — the rank is the number of commas plus one. A
type carries at most one bracket suffix group: a second group, as
in `int[][]`, is a compile-time error. A jagged shape — an array
whose elements are arrays of different lengths — is not
expressible.

Every array value's type is the standard library's generic class
`Array<T>`: the bracket spelling is an *annotation* of that one
type, and `int[]` and `Array<int>` are the same type at every
compile-time check — assignment, argument passing, overload
resolution, container elements. The annotation is a promise the
compiler keeps (static coordinate checks, a constant `rank()`,
direct subscript code); the bracket-free spelling `Array<int>`
makes no rank promise, and the rank then lives in the value
(§4.10.7). Writing the element as a type argument gives the same
jagged rejection in generic clothes: `Array<Array<int>>` is
`int[][]` and is a compile-time error, while a container of array
references — `List<Array<int>>`, `Array<List<int>>` — stays legal
(§4.12.5).

## 4.10.2 Creation

An array value is created with a length for each dimension, and
those lengths never change for the value's lifetime. Every element
starts as the default value of the element type. Each creation
dimension is an integer expression — a literal, a variable, a call
result, any expression whose value is an integer; an enumeration
member counts as its integer value, and a floating-point-valued
dimension is a compile-time error. It is a run-time error if a
creation length is negative or if the product of the lengths
exceeds 100000000; the error is raised at the creation site as an
`IndexOutOfBoundsException` (chapter 8).

A value whose rank is itself a run-time fact is created through
the generic spelling: the target type fixes the element type, and
the dimensions arrive as a rank-1 `int[]` whose length is the
rank.

```nlang
int[] dims = int[2];
dims[0] = 2; dims[1] = 3;
Array<int> m = Array(dims);   // rank 2, 2x3, zero-filled
Array<float> w = Array(dims); // element type comes from the target
```

The rank — the length of `dims` — must fall in the legal range
(1 to 64) and is validated when the value is created; a length of
0 or above 64 raises a catchable `IndexOutOfBoundsException`. The
`dims` argument must be a rank-1 `int` array: any other element
type is a compile-time error.

```nlang
int[] a = int[3];         // expression position: fixed length, elements zero-initialized
int[] b = [1, 2, 3];      // bare bracket literal
int[] e = [];             // empty array literal (int[0] is equivalent)

int d[3];                 // declaration position, fixed length: zero-filled
int f[100] = [7, 8];      // declaration position, partial init: rest zero-filled
```

- In **expression position**, `int[3]` creates the array and
  `[1, 2, 3]` is the collection-literal form; the brace form
  `{...}` is reserved for collection, struct and class initializers
  and is rejected for array targets (§4.10.8).
- In **declaration position**, `int d[3];` declares a fixed-length
  array directly; with an initializer, elements not listed keep
  their defaults. Every declarator of a comma-separated list may
  carry its own size (§3.8).
- A declaration without an initializer (`int[] g;`) yields `null`
  (§3.8).
- The element type may be a struct (each element is an independent
  value, §4.7.4) or a class (references are stored, §4.8.1).

## 4.10.3 Reference semantics

An array value is a reference type: assignment copies the
reference, and two variables then share the same elements. The
callee's element writes are visible to the caller through a passed
array, and an array field or element stores the reference.

Reading an element of struct type yields a copy of the stored
struct, not the stored struct itself; writing a field of that copy
does not change the array, and storing a struct back requires an
assignment to the element (§4.7.4, chapter 9).

An array value is also an `Object` (§4.11): either spelling flows
into an `Object` position without allocation, and the protocol
calls `toString`, `equals` and `getHashCode` reach the array's own
implementations — `toString` renders the element list
(`"[7, 0, 0, 0]"`), the other two compare identity (§4.11.2).

## 4.10.4 Members: length, rank, shape

Every array value supports three members: `length()` returns the
total number of elements as an `int`; `rank()` returns the rank;
`shape()` returns the length of each dimension. They are methods,
with no arguments, consistent with `string.length()` (§4.3).

*Informative:* the array members (`length`, `rank`, `shape`,
`toString`, `equals`, `getHashCode`) are declared by the generic
face class `Array<T>` in the `lang` package of the standard
library. The class holds only the declarations — it cannot be
extended (a generic instantiation is not a legal parent class,
§4.12) — but unlike `String` it is a real type: `Array<int>` works
everywhere a type is written, and its values are constructed with
`Array(dims)` or the bracket creation forms of §4.10.2. The bare
name `Array` without type arguments stays a compile-time error.

## 4.10.5 Subscripting

`a[i]` reads and writes an element; the index is an integer
expression. A subscript may also carry slice items, which produce
views — §4.10.9.

```nlang
int[] a = int[3];
a[0] = 9;
a[1] = a[0] + 1;
int x = a[a.length() - 1];
```

A subscript left-value takes compound assignment (`a[i] += 1`)
under the same rules as any other left-value (chapter 5); for
struct elements the write goes to the array's own element
(§4.10.3).

On a bracket-annotated array the coordinate count is a
compile-time check (§4.10.7). On a value typed `Array<T>` without
brackets the source still writes a fixed coordinate count, and the
count is checked against the value's rank when the subscript runs:
a mismatch raises a catchable `IndexOutOfBoundsException` whose
message names both counts. There is no coordinate-count
array allocation — the check rides the same subscript operation.

```nlang
int[] dims = int[2];
dims[0] = 2; dims[1] = 3;
Array<int> m = Array(dims);           // rank 2
m[1, 2] = 9;                          // two coordinates, rank 2: fine
int v = m[0];                         // run-time error: 1 coordinate vs rank 2
```

## 4.10.6 Out of bounds

An index outside `0` to the dimension's length minus one is a
catchable run-time error on both reads and writes, raised as an
`IndexOutOfBoundsException` (chapter 8).

*Informative:* the creation cap of §4.10.2 covers every creation
path — `T[n]` and the tensor package's native minting alike; 100M
elements is 800 MB at the widest element.

## 4.10.7 Multi-dimensional arrays

Rectangular multi-dimensional arrays are written `T[,]` (two
dimensions), `T[,,]` (three), up to rank 64 (§4.10.1). The
dimension count is part of the type: `int[,]` and `int[]` are
unrelated types, and assignment or parameter passing between them
is a compile-time error.

```nlang
int[,] grid = int[2, 3];      // 2x3, zero-filled
grid[1, 2] = 7;               // comma subscript read/write
int rows = 2;
int[,,] t = int[rows, 1, 2];  // sizes may be expressions (runtime)
```

- The coordinate count of `a[i, j]` must equal the rank — fewer or
  more coordinates is a compile-time error — and each coordinate is
  an integer expression; an out-of-range coordinate raises
  `IndexOutOfBoundsException` exactly as in the rank-1 case
  (§4.10.6). (On a bracket-free `Array<T>` value the coordinate
  count is a run-time check, §4.10.5.)
- `a.rank()` returns the rank; `a.shape()` returns a rank-1 `int`
  array holding the length of each dimension; `a.length()` returns
  the total element count, the product of the dimensions
  (§4.10.4).
- `foreach` iterates the elements in row-major order, the last
  dimension varying fastest; the loop variable type is the element
  type (chapter 6).
- An array type works as a parameter, return type, field and
  container element type, and survives module imports unchanged.
  Reference semantics and the `string` destination rules match the
  rank-1 case (§4.10.3).

Declaration-site sizes are rank-1 only (`int b[100]`); a
multi-dimensional array uses the creation form — `int a[2, 3];` is
a compile-time error.

### Flowing between annotated and bracket-free spellings

A value typed with brackets flows into a bracket-free
`Array<T>` position freely — the annotation says more, not
something else. The reverse direction carries a promise the value
has not made, so the compiler inserts an implicit *boundary rank
check* — one check at each assignment, argument-passing and return
boundary, not on every access. When the value's rank does not
match the annotation, the check raises a catchable
`InvalidCastException` whose message names the expected and actual
ranks; `null` flows through as `null` (§4.5). The explicit form of
the same check is the cast `a as int[,]` (§4.5).

```nlang
int[,] grid = int[2, 3];
Array<int> dyn = grid;        // annotated -> bracket-free: free
int[] row = dyn;              // boundary check: rank 2 vs 1, throws
int[,] back = dyn as int[,];  // explicit form of the same check
```

A mismatch between two bracket spellings (`int[]` into `int[,]`)
remains a compile-time error: both sides have made their promise.

## 4.10.8 Initializers

An array initializer is a bracket literal. For a rank-1 target the
entries are element expressions; for a higher rank the literal
nests as deep as the target's dimension count, and the shape is
validated at compile time to be rectangular: nesting depth equal to
the dimension count, equal row widths within each level. Elements
land in the new array in row-major order.

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,,] t = [[[1, 2]], [[3, 4]]];
double half = 0.5;
double[,] w = [[half, half * 2], [0.0, 1.0]];   // elements may be expressions
```

A jagged literal — rows of unequal width within a level — and a
nesting depth that does not match the dimension count are
compile-time rejections. At dimension count 2 or more the literal
must be non-empty: no row means no dimensions to infer (the rank-1
`int[] a = [];` form stays legal).

An entry above the leaf level may also be an expression whose value
is an array of exactly one dimension below the target — a function
result, a variable, any array-valued expression — and such
expression rows mix freely with bracket rows at the same level.

```nlang
int[] makeRow(int w) {
    int[] r = int[w];
    for (int i = 0; i < w; i += 1) { r[i] = i; }
    return r;
}

int[,] m = [makeRow(2), [3, 4]];    // expression row + bracket row
```

With an expression row present the shape is a run-time fact: the
first row pins the inner dimensions, and every later row must agree
with it. Rows of differing width raise a catchable base `Exception`
naming the level and both widths; a `null` row raises a
`NullPointerException` (chapter 8). Without expression rows the
whole shape remains a compile-time fact.

The brace form `{...}` is not accepted for an array target.

*Informative:* the full initializer-list rules, including the
shapes a literal may target, are in [Collection
Initializers](../05-expressions/initializers.md).

## 4.10.9 Slices and views

A subscript may carry *slice items* in place of plain indices: on
each axis, `start:stop:step` with every part optional (§2.4). The
result is not a copy but a *view* — a new array value whose
elements are the source's elements at the selected positions.

```nlang
int[,] m = int[4, 4];
int[,] tl = m[0:2, 0:2];      // the 2x2 top-left corner
int[] row = m[1, :];          // plain index drops its axis: rank 1
int[] rev = m[0, ::-1];       // step -1 walks the axis backwards
```

The rank of the result is the number of sliced axes: plain indices
select one position along their axis and remove it from the shape,
slice items keep their axis with the selected length. A rank-1
array sliced (`a[1:3]`) yields a rank-1 view; an all-index
subscript (`m[0, 1]`) is the element access of §4.10.5 and no view
is created.

Each slice axis normalizes independently, Python-style. Omitted
endpoints take defaults fixed by the step's sign — `0` and the
axis length for a positive step, the last position and one before
the first for a negative one. A negative endpoint counts from the
end (`-1` is the last position); endpoints are clipped to the
axis; a `0` step is a run-time error, raised as a catchable base
`Exception` (§8.3). An empty selection is legal and yields a view
with a zero length on that axis.

A view is an ordinary array value of the element type of its
source: assignment copies the view reference, the members of
§4.10.4 report the view's own shape and count, `toString` renders
the selected elements, and every flowing rule of §4.10.7 applies
unchanged. What makes it a view is *aliasing*: element reads and
writes go through to the source array —

```nlang
int[] a = [1, 2, 3, 4];
int[] mid = a[1:3];
mid[0] = 9;                   // writes a[1]
a[2] = 7;                     // visible through mid[1]
```

— and views of views compose: slicing a view addresses the
original elements, not a copy of the copy. A slice expression
itself may not be the target of an assignment (`a[1:3] = x` is a
compile-time error): the view is a value, and stores go through
its own subscripts, one element at a time.

An axis can select every position of the source through a
broadcast view (`broadcastTo`, §18.2) or a step that repeats it:
such an axis has length in the view but addresses one source
position. Reads repeat the source element; stores are rejected —
two logical cells naming one source cell would silently corrupt
one of the writes — raising a catchable base `Exception` naming
the axis. Negative-stride axes carry no such alias and stay
writable.

*Informative:* a view is implemented as its recorded shape,
per-axis strides and an offset into the source's elements; the
length of a slice view on one axis never depends on the others.
Writing a view through `writeObject` writes its *logical
content* as a fresh record — the round trip yields an independent
owning array (§14.5).
