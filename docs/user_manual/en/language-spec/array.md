# Array


Arrays are fixed-length, index-addressable sequences. The syntax is `T[]`.

### Creating arrays

```nlang
int[] a = int[3];         // expression position: fixed length, elements zero-initialized
int[] b = [1, 2, 3];      // bare bracket literal
int[] e = [];             // empty array literal (int[0] is equivalent)

int d[3];                 // declaration position, fixed length: zero-filled
int f[100] = [7, 8];      // declaration position, partial init: rest zero-filled
```

- **Expression position**: `int[3]` creates a fixed-length heap array with
  zero-initialized elements; `[1, 2, 3]` is the collection literal form
  (the brace form `{...}` is reserved for collection, struct, and class
  initializers — `int[] c = {1, 2};` is rejected). Inference and
  disambiguation rules are in
  [Collection Initializers](collection-initializers.md).
- **Declaration position**: `int d[3];` declares a fixed-length array
  directly, zero-filled; with an initializer, elements not listed are
  zero-filled (`int f[100] = [7, 8];` fills only the first two). Every
  declarator of a comma-separated list may carry its own `[n]` size
  (`int a, b[3];` is legal); how each declarator's type combines the head
  type with its own brackets is covered in
  [Declarations](declarations.md).
- Declaring without an initializer (`int[] g;`) yields a null reference —
  see the default value table in [Declarations](declarations.md).
- The element type can be a struct (`Point[3]` materializes an independent
  value per element, see [Struct](struct.md)) or a class (references are
  stored). Arrays of arrays (jagged form) are not supported — see
  [Known Limitations](known-limitations.md).

### Reference semantics

An array is a **reference type**: the array lives on the heap, and assignment
and parameter passing copy the array **reference**, not the elements. Two
variables pointing at the same array observe the same element writes.

- **Assignment**: `b = a` makes `b` refer to the same array object.
- **Parameter passing**: the reference is passed; the callee's element writes
  are visible to the caller.
- **Return value**: the reference is returned.
- **As a field / element**: stored as a reference (see
  [Struct](struct.md), [Class](class.md),
  [Built-in Generic Classes](builtin-generic-classes.md) for `List<int[]>`).

A struct-typed array is the exception in behavior: each element is a value
type, so `Point[n]` materializes a fresh independent `Point` per element
and reading an element deep-copies it. See [Struct](struct.md) "Value
semantics". The full value-vs-reference summary is on
[Type Semantics](type-semantics.md).

### Length

`a.length()` is a **method** (no arguments, returns `int`), consistent with
`string.length()` and the built-in containers. See [String](string.md).

### Subscript read/write

`arr[i]` reads and writes an element; the index is an expression (may be
nested):

```nlang
int[] a = int[3];
a[0] = 9;
a[1] = a[0] + 1;
int x = a[a.length() - 1];
```

Compound assignment on a subscript left-value (`arr[i] += 1`) follows the
same rules as [Compound Assignment](compound-assignment.md) (for string
elements, only `+=`).

### Out of bounds

An out-of-range index throws `IndexOutOfBoundsException` on both reads and
writes (a runtime error, catchable by `try/catch`; if uncaught the program
terminates with exit code 1 — see [Exceptions](exception.md) for the built-in
exception table).

A sized creation is bounded the same way: a negative length, or one past
100 million elements, throws `IndexOutOfBoundsException` at the creation
site. The cap covers every creation path — `T[n]` and the tensor package's
native minting alike (100M elements is 800MB at the widest element).

### Where an array value may go

Rules for where an array value may go in scalar contexts: see
[Known Limitations](known-limitations.md), "Array values in scalar contexts".
Iterate an array's elements with `foreach` (see [Foreach](foreach.md)).
Initialize one with a literal (see [Collection Initializers](collection-initializers.md)).

### Multi-dimensional arrays

Rectangular multi-dimensional arrays are written `T[,]` (two
dimensions), `T[,,]` (three), up to 8. The dimension count is part of
the type: `int[,]` and `int[]` are unrelated types — assignment or
parameter passing between them is rejected at compile time.

```nlang
int[,] grid = int[2, 3];      // 2x3, zero-filled
grid[1, 2] = 7;               // comma subscript read/write
int rows = 2;
int[,,] t = int[rows, 1, 2];  // sizes may be expressions (runtime)
```

- **Subscript**: the coordinate count of `a[i, j]` must equal the
  dimension count — fewer or more coordinates is a compile error;
  each coordinate is an expression, and an out-of-range coordinate
  throws `IndexOutOfBoundsException` exactly like the 1-D case.
  Compound assignment follows the same rules as 1-D subscripts.
- **Members**: `a.rank()` returns the dimension count; `a.shape()`
  returns a 1-D `int` array holding the element count of each
  dimension; `a.length()` returns the total element count (the
  product of the dimensions).
- **foreach**: iterates the elements in row-major order (the last
  dimension varies fastest); the loop variable type is the element
  type.
- **Parameters and storage**: `int[,]` works as a parameter, return
  type, field, and container element type, and survives module
  imports unchanged; reference semantics and the `string` destination
  rules match the 1-D case (see
  [Known Limitations](known-limitations.md)).

Initializers are nested bracket literals, nested as deep as the
target's dimension count; the shape is validated at compile time to
be rectangular — nesting depth equal to the dimension count, equal
row widths within each level:

```nlang
int[,] m = [[1, 2], [3, 4]];
int[,,] t = [[[1, 2]], [[3, 4]]];
double half = 0.5;
double[,] w = [[half, half * 2], [0.0, 1.0]];   // elements may be expressions
```

Elements land in the new array in row-major order. Jagged literals
(rows of unequal width within a level) and a nesting depth that does
not match the dimension count are named compile-time rejections; at
dimension count >= 2 the literal must be non-empty — no row to infer
from means no dimensions (the 1-D `int[] a = [];` form stays legal).
The brace form keeps its existing rejection for array targets (see
[Collection Initializers](collection-initializers.md)).

Declaration-site sizes are one-dimensional only (`int b[100]`);
multi-dimensional arrays use `int[2, 3]` creation — `int a[2, 3];`
is rejected with a named diagnostic pointing at the creation form
(class fields likewise need a creation initializer).
