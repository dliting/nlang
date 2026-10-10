# Array


Arrays are fixed-length, index-addressable sequences. The syntax is `T[]`.

```nlang
int[] a = new int[3];    // fixed-length heap array, elements zero-initialized
int[] b = [1, 2, 3];     // bare initializer (see Collection Initializers)
int[] c = new int[0];    // empty array
```

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
type, so `new Point[n]` materializes a fresh independent `Point` per element
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
int[] a = new int[3];
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

### Where an array value may go

Rules for where an array value may go in scalar contexts: see
[Known Limitations](known-limitations.md), "Array values in scalar contexts".
Iterate an array's elements with `foreach` (see [Foreach](foreach.md)).
Initialize one with a literal (see [Collection Initializers](collection-initializers.md)).
