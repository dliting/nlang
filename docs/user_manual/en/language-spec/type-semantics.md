# Type Semantics


Every type in NLang has defined semantics for four operations: **assignment**
(`a = b`), **parameter passing**, **return value**, and use as a **field or
array element**. This page is the complete per-type summary; each row links
to the page that explains that type in detail.

### Summary table

| Type        | Kind              | Assignment       | Parameter passing | Return value        | As field / element  |
|-------------|-------------------|------------------|-------------------|---------------------|---------------------|
| `byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong` | value | copy | copy | copy | copy |
| `float` `double` | value         | copy             | copy              | copy                | copy                |
| `bool` `char` | value            | copy             | copy              | copy                | copy                |
| `string`    | value (immutable)  | copy handle      | copy handle       | copy handle         | copy handle         |
| `enum`      | value (int32)      | copy             | copy              | copy                | copy                |
| `struct`    | value (deep copy)  | deep copy        | deep copy         | deep copy           | deep copy (owned)   |
| `class`     | reference          | copy reference   | pass reference    | return reference    | store reference     |
| `interface` | reference          | copy reference   | pass reference    | return reference    | store reference     |
| `T[]`       | reference (heap)   | copy reference   | pass reference    | return reference    | store reference     |
| `List<T>`   | reference          | copy reference   | pass reference    | return reference    | store reference     |
| `Dict<K,V>` | reference          | copy reference   | pass reference    | return reference    | store reference     |
| `Object`    | reference (boxed)  | copy reference   | pass reference    | return reference    | store reference     |
| `Func`      | value (func ref)   | copy             | copy              | copy                | copy                |

### Per-type notes

- **The 12 scalar primitives** (integer family, `float`/`double`, `bool`,
  `char`) — value types. Assigned, passed, and returned by value; the
  numeric family participates in arithmetic promotion and the conversion
  matrix (below). See [Primitives](primitives.md).
- **`string`** — value semantics *via* an immutable interned object: the
  handle is copied, but the object's content never changes, so "sharing" is
  harmless. `==` compares content, not identity. A null string handle (0)
  reads as the empty string `""`. See [String](string.md).
- **`enum`** — value type backed by int32. Assigned, passed, and returned by
  value; compared by its integer value; there is no independent object
  identity. See [Enum](enum.md).
- **`struct`** — value type with **deep-copy** semantics: copying copies the
  whole aggregate, including nested struct fields. The one exception is a
  class-typed field inside the struct, which is shallow-copied (the reference
  is shared). See [Struct](struct.md).
- **`class`** — reference type. Assignment/passing/return copy the reference
  (heap index); both names point at the same object. A null reference throws
  `NullPointerException` on member access. See [Class](class.md).
- **`interface`** — reference type like `class`; a value of interface type
  refers to the implementing object. See [Interface](interface.md).
- **`T[]`** — reference type: the array lives on the heap; assignment and
  passing copy the array *reference*, not the elements. See [Array](array.md).
- **`List<T>` / `Dict<K,V>`** — reference types; assignment and passing copy
  the container reference. See [Built-in Generic Classes](builtin-generic-classes.md).
- **`Object`** — reference type that may hold a boxed primitive or a class
  reference; assignment copies the reference. See [Object & Boxing](object.md).
- **`Func`** — a first-class value: the function reference is copied by
  value. See [Functions](functions.md).

### Numeric conversion matrix

Conversions between numeric types come in three tiers: **implicit** (the
compiler inserts them), **implicit with a lossy warning**, and **explicit
`as`** (user-acknowledged, no warning). The complete rules and examples
are on [Primitives](primitives.md) and [Type Casts](type-casts.md).

**Implicit (no warning)**:

- Same-sign rank widening: `byte→short→int→long`;
  `ubyte→ushort→uint→ulong`.
- Cross-sign range containment: `ubyte→short/int/long`;
  `ushort→int/long`; `uint→long`.
- Integer→float where the target represents the source range exactly:
  narrow integers (byte..ushort) → `float/double`; `int/uint` and
  narrower → `double`.
- Float widening: `float→double`.
- `char→string` (encoded as that code point's Unicode Transformation Format (UTF-8) bytes);
  `enum→int` (its backing type).

**Implicit with a lossy warning** — when the target's range cannot hold
the source's, the compiler emits `implicit conversion from 'X' to 'Y'
loses precision` (`ncc --no-warn` suppresses all warnings):

- `int/uint/long/ulong → float`
- `long/ulong → double`

Constant operands whose value is exactly representable are exempt
(`float f = 5;` warns nothing) — the warning targets "the actual loss of
this conversion", not "the possible loss of the type pair".

**Constant fit**: an integer **literal** assigned to a narrower target is
implicitly legal when the value is in the target's range
(`byte b = 5;`); it covers only the literal itself, not folded
expressions (`byte b = 1 + 2;` still needs `as`). Double literals into a
`float` target follow the same rule. See
[Primitives](primitives.md).

**Explicit `as`**: all narrowing (`float→int`, `double→float`,
`long→int`, `ulong→long`, ...), reverse cross-sign (`short→ubyte`, ...),
and `numeric↔char`. See [Type Casts](type-casts.md).

**Forbidden**: `bool` ↔ anything; `string → numeric` via `as` (use the
`toInt()` family).

**Arithmetic promotion**: integer operands narrower than int are first
promoted to int; the result type is the smallest type that can implicitly
receive both operands (`int + uint` → `long`; mixing `int`/`long` with
`ulong` is a compile error); when floating point participates, the larger
float rank wins. bool and char take no part in arithmetic. See
[Operators](operators.md).

### Why the two families differ

The split is value vs. reference:

- **Value types** (the 12 scalar primitives, `enum`, `string`, `struct`,
  `Func`) are copied on assignment. Two variables hold independent data;
  mutating one never affects the other. (`string` is a value type even
  though it is an object, because the object is immutable.)
- **Reference types** (`class`, `interface`, array, `List`, `Dict`, `Object`)
  share the underlying object. Assignment copies the reference, so both names
  observe the same object and the same mutations.

The reference family shares one rule: **a null reference throws
`NullPointerException` on member access** (class, interface, array, and
container nulls all behave the same). See [Class](class.md) for the null
semantics.
