# Types


### Primitive Types

| Type   | Size  | Description          |
|--------|-------|----------------------|
| int    | 4 bytes | 32-bit signed integer |
| float  | 4 bytes | 32-bit IEEE 754 float |
| string | 4 bytes | Reference to string pool entry |

**Numeric literals**: Integer literals may use exponent notation — `2e5`
is 200000. The value must be integral and fit the 32-bit integer range;
`1e30` (out of range) and `2e-1` (= 0.2, fractional) are compile errors.
Float literals require a decimal point and may use exponents (`1.0e30`,
`2.5e-3`); a bare `1e30` is an int literal, not a float.

**String encoding**: String literals are stored as their UTF-8 byte sequence
in the module string pool. `string.length()` returns the **byte count**, not
the Unicode code-point count — `"héllo".length()` is 6 (5 code points but `é`
is 2 bytes in UTF-8). Proper UTF-8 code-point iteration is deferred to a
future phase.

### Composite Types

| Type   | Semantics | Storage          | Description              |
|--------|-----------|------------------|--------------------------|
| enum   | Value     | int32            | Named integer constants  |
| struct | Value     | Heap index (copy-on-assign) | Value-typed aggregate |
| class  | Reference | Heap index       | Reference-typed object   |

### Null

Class-typed variables can be null (represented as heap index 0). Accessing
fields or methods on null throws a `NullPointerException` (catchable via
try/catch since Phase 9d).
