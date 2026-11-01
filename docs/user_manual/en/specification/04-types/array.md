# 4.10 Arrays

## 4.10.1 Element type and rank

An array type is fixed by its *element type* and its *rank*: the
number of dimensions, at least 1 and at most 64. The rank is part
of the type: `int[]` and `int[,,]` are different types, and so are
`int[]` and `long[]`. A rank above 64 is a compile-time error.

The rank is written as a single bracket suffix group after the
element type (§2.3): `[]` is rank 1, `[,]` is rank 2, `[,,]` is
rank 3, and so on — the rank is the number of commas plus one. A
type carries at most one bracket suffix group: a second group, as
in `int[][]`, is a compile-time error. A jagged shape — an array
whose elements are arrays of different lengths — is not
expressible.

## 4.10.2 Creation

An array value is created with a length for each dimension, and
those lengths never change for the value's lifetime. Every element
starts as the default value of the element type. It is a run-time
error if a creation length is negative or if the product of the
lengths exceeds 100000000; the error is raised at the creation
site as an `IndexOutOfBoundsException` (chapter 9).

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
assignment to the element (§4.7.4, chapter 10).

## 4.10.4 Members: length, rank, shape

Every array value supports three members: `length()` returns the
total number of elements as an `int`; `rank()` returns the rank;
`shape()` returns the length of each dimension. They are methods,
with no arguments, consistent with `string.length()` (§4.3).

*Informative:* the array members (`length`, `rank`, `shape`,
`toString`) are declared by the prelude class `Array` in the
standard library; like `String`, that class only holds the
declarations — it cannot be instantiated, extended or used as a
type, and array types are written with the bracket syntax.

## 4.10.5 Subscripting

`a[i]` reads and writes an element; the index is an integer
expression.

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

## 4.10.6 Out of bounds

An index outside `0` to the dimension's length minus one is a
catchable run-time error on both reads and writes, raised as an
`IndexOutOfBoundsException` (chapter 9).

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
  (§4.10.6).
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
`NullPointerException` (chapter 9). Without expression rows the
whole shape remains a compile-time fact.

The brace form `{...}` is not accepted for an array target.

*Informative:* the full initializer-list rules, including the
shapes a literal may target, are in [Collection
Initializers](../05-expressions/initializers.md).
