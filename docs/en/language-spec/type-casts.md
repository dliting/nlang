# Type Casts


There are two ways to convert a value between types: the explicit `as`
operator (scalar narrowing, numeric↔char, unboxing, class downcast), and
implicit coercion to `string`. Which conversions between scalar-family
members are implicitly legal (widening, range containment, integer→float)
is decided by the conversion matrix — see
[Type Semantics](type-semantics.md).

### Explicit cast (`as`)

```nlang
expr as TypeName
```

`as` is NLang's only explicit conversion form (there is no C-style
`(T)expr` prefix cast — a prefix form cannot be told apart from a
parenthesized expression reliably; the parser cannot distinguish
`(foo) + bar` from `(foo + bar)`. The keyword operator has no such
ambiguity, matching C#, TypeScript, and Kotlin). Four classes of
conversion are supported:

- **Scalar narrowing and cross-sign** (bit-truncating, the C# unchecked
  equivalent): `d as int` (double→int), `d as float` (double→float),
  `l as int` (long→int), `s as ubyte` (reverse cross-sign) — any scalar
  pair the implicit matrix does not admit can be made explicit with `as`.
  Explicit means user-acknowledged: **no lossy-conversion warning is
  emitted**.
- **Numeric ↔ char**: `c as int` reads the code point; `65 as char`
  constructs a char explicitly, validating the scalar value at run time
  (a surrogate or a value above U+10FFFF throws the runtime error
  `value is not a valid Unicode scalar value`).
- **Unboxing**: `o as int` / `o as long` / `o as string` — unwrap a boxed
  primitive; all 12 scalars can be unboxed. Throws if `o` is null or the
  boxed type tag doesn't match.
- **Class downcast**: `o as SubClass` — verify the runtime class of `o` is
  `SubClass` or a subclass thereof. Throws on mismatch. `o as Object` is a
  no-op (allowed for symmetry).

`as` is reserved for pairs the matrix does not admit — using `as` on a
conversion that is **already implicit** is also a compile error
(`` `as` cannot perform implicit conversion `UByte` → `Int32` ``):
`ubyte`→`int` is implicit widening anyway; writing `b as int` is
redundant — plain `int x = b;` works. The implicit matrix is on
[Type Semantics](type-semantics.md).

**Forbidden `as`**:

- `string → numeric`: `"5" as int` is a compile error (`` Invalid cast:
  `String as Int32` is not allowed ``) — use the standard library's
  `s.toInt()` / `s.toFloat()` / `s.toLong()` / `s.toDouble()` family (see
  [Standard Library](standard-library.md)).
- `bool ↔ anything`: bool takes part in no conversion.
- Array operands are rejected outright (`ia as int`, `ia as Object` —
  "the cast operand is an array"): an array value's legal conversions are
  its own array type and string targets — never `as` (see
  [Known Limitations](known-limitations.md) for the full array-value
  conversion rule).

The Object side of unboxing/downcasting is on
[Object & Boxing](object.md).

### Primitive → string coercion

When a scalar primitive (any of the 12) appears where a string is
expected, NLang automatically coerces it to its string form. Most common
in concatenation, but it also fires on direct assignment and field
storage.

```nlang
string s1 = "x" + 5;       // "x5" — int coerced to "5"
string s2 = 5 + "x";       // "5x" — symmetric
string s3 = "x=" + 2.5;    // "x=2.5" — double shortest round-trip
string s4 = "x" + 'y';     // "xy" — char coerced to its UTF-8 encoding
string s5 = "ok=" + true;  // "ok=true" — bool coerced to true/false
string s6 = "n=" + 5000000000;   // "n=5000000000" — long printed directly
string s7 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
```

**Rendering rules**: bool → `true`/`false`; char → the UTF-8 bytes of its
code point (1–4 bytes); the integer family prints in decimal as-is
(narrow integers and long/ulong alike); float and double use
**shortest round-trip** rendering — the shortest decimal representation
for which `parse(format(x)) == x` holds (`0.5` rather than `0.500000`;
`0.1` prints as `0.1`, not the internal binary approximation). print,
string methods, and interpolation share the same conversion point.

**Implementation**: all scalar→string conversions share one generalized
instruction, `OP_Prim_to_str <kind>` (the kind immediate selects one of
the 12 scalar renderer rows); enum and arrays keep their dedicated
instructions. `string → numeric` remains rejected — see "Forbidden `as`"
above.

### Object.toString() protocol

All class instances inherit `string toString()` from `Object`. The default
implementation returns `"ClassName@heapIdxHex"` (e.g. `"Point@7"`,
`"Point@ff"`). User classes override it by declaring `string toString()
{ ... }` — dispatched by name, like `equals`/`getHashCode`. See
[Object & Boxing](object.md).

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

Dispatch matrix:

| Receiver          | `.toString()` result          | Overridable? |
|-------------------|-------------------------------|--------------|
| class (user override) | user-defined             | yes |
| class (no override) | `"ClassName@hex(heapIdx)"`   | no (Object built-in) |
| enum              | enum member name (e.g. `"Red"`) | no |
| all scalar primitives | same rendering as the →string coercion | no |
| string            | itself (identity)             | no |

**Implicit coercion**: `"x" + obj` automatically calls `obj.toString()`,
as in Java/C#. This applies to class, enum, and all scalar receivers.
struct receivers are **permanently excluded** — `"x" + structInstance`
is a compile error (struct is NLang's plain-data type; use class for
object semantics). See [Struct](struct.md).

**Enum name output**: `Color.Red.toString()` returns `"Red"` (not `"0"`).
The compiler embeds one name table per enum; the VM looks the member name
up by value with `OP_Enum_to_str`. Out-of-range enum values throw at run
time. See [Enum](enum.md).

**String identity**: `"hello".toString()` returns `"hello"` — the
compiler folds this to a no-op (no instruction emitted).

**Limitations**:
- No warning when implicit coercion happens (silent, like Java)
- `struct.toString()` / `"x" + structInstance` — permanently rejected
