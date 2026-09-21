# Types


### Primitive Types

| Type   | Size  | Description          |
|--------|-------|----------------------|
| int    | 4 bytes | 32-bit signed integer |
| float  | 4 bytes | 32-bit IEEE 754 float |
| string | 4 bytes | Handle (1-based) into the string object store (0 = null, reads as "") |

**Numeric literals**: Integer literals may use exponent notation — `2e5`
is 200000. The value must be integral and fit the 32-bit integer range;
`1e30` (out of range) and `2e-1` (= 0.2, fractional) are compile errors.
Float literals require a decimal point and may use exponents (`1.0e30`,
`2.5e-3`); a bare `1e30` is an int literal, not a float.

**String encoding**: String literals are stored as their UTF-8 byte sequence
in the module string constant table. `string.length()` returns the **byte count**, not
the Unicode code-point count — `"héllo".length()` is 6 (5 code points but `é`
is 2 bytes in UTF-8). Proper UTF-8 code-point iteration is deferred to a
future release.

### Composite Types

| Type   | Semantics | Storage          | Description              |
|--------|-----------|------------------|--------------------------|
| enum   | Value     | int32            | Named integer constants  |
| struct | Value     | Heap index (copy-on-assign) | Value-typed aggregate |
| class  | Reference | Heap index       | Reference-typed object   |

### String Implementation and Memory Semantics

Strings are **immutable objects** at run time: operations like
concatenation and substring extraction produce new content, and an
existing object's content never changes. A string slot holds a handle
into the string object store; handle 0 means null and reads as the
empty string.

- **Constants live as long as the execution**: string literals from the
  source are materialized as immortal objects when the module starts
  executing and are never collected during the run.
- **Short-string interning**: runtime strings up to 40 bytes are
  interned by content — identical content shares one object. `==` still
  compares content; the semantics are unchanged.
- **O(1) appends**: `s = s + x` first records a concatenation node and
  flattens it into real content on first read, so appending in a loop
  never copies the whole string step by step.
- **Bounded memory**: unreferenced string objects are reclaimed by the
  garbage collector, so long-running programs (prompt-building loops and
  similar) do not grow memory without bound.

### Null

Class-typed variables can be null (represented as heap index 0). Accessing
fields or methods on null throws a `NullPointerException` (catchable via
try/catch).
