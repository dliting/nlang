# Type Casts


Three ways to convert a value between types: the C-style `(T)` cast, the
runtime-checked `as` operator, and implicit coercion to `string`.

### Explicit `(T)` cast

```nlang
int x = 5;
float y = (float)x;
int z = (int)y;
```

Explicit casts between int and float. Implicit widening (int→float) is
allowed in some contexts (see [Primitives](primitives.md)).

### Runtime-checked cast (`as`)

```nlang
expr as TypeName
```

The following runtime-checked conversions are supported:

- **Unbox**: `o as int` / `o as float` / `o as string` — unwrap a boxed
  primitive. Throws if `o` is null or the boxed type tag doesn't match.
- **Class downcast**: `o as SubClass` — verify the runtime class of `o` is
  `SubClass` or a subclass thereof. Throws on mismatch.
- **Identity / upcast**: `o as Object` — no-op (any class is already
  Object). Allowed for symmetry.

Type-incompatible casts (`5 as string`, `o as int` when `o` holds a class ref)
are compile errors — `as` only permits same/box/unbox/downcast. Array operands
are rejected outright (`ia as int`, `ia as Object` — "the cast operand is an
array"): an array value's legal conversions are its own array type and string
targets in every position — never `as` (see
[Known Limitations](known-limitations.md) for the full array-value conversion
rule).

The `as` keyword was chosen over C-style `(T)expr` prefix cast for unboxing
and class casts because `(T)expr` cannot be reliably distinguished from
parenthesized expressions (the parser cannot tell `(foo) + bar` from
`(foo + bar)`). Keyword operators like `as` have no such ambiguity — this
matches the approach taken by C#, TypeScript, and Kotlin. See
[Object & Boxing](object.md) for the Object side of unbox/downcast.

### Primitive → String Coercion

When a primitive (int or float) appears in a context expecting string, NLang
auto-coerces it to its decimal string form. This is most common in string
concatenation, but also fires in direct assignment and field stores.

```nlang
string s1 = "x" + 5;       // "x5" — int coerced to "5"
string s2 = 5 + "x";       // "5x" — symmetric
string s3 = "x=" + 2.5;    // "x=2.5" — float uses %g format
string s4 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
string s5 = 42;            // "42" — direct assignment path
string s6 = "x" + (-7);    // "x-7" — negative formatted with sign
```

**Implementation**:
- `int → string`: `OP_Int32_to_str` (decimal, via `std::to_string`)
- `float → string`: `OP_Float_to_str` (`%g` format — `2.5` not `2.500000`)
- Both mint the formatted string as a runtime string object and write the new
  handle into the result slot; `OP_Assign` then moves the result into the
  destination slot.
- `string → int/float` remains rejected — use the standard library's
  `s.toInt()` / `s.toFloat()` instead (see [Standard Library](standard-library.md)).

### Object.toString() Protocol

All class instances inherit `string toString()` from `Object`. The default
implementation returns `"ClassName@heapIdxHex"` (e.g. `"Point@7"`,
`"Point@ff"`). User classes override it by declaring `string toString() { ... }`
— virtual dispatch by name, same as `equals`/`getHashCode`. See
[Object & Boxing](object.md).

```nlang
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

Dispatch matrix:

| Receiver | `.toString()` result | Override? |
|----------|---------------------|-----------|
| class (user override) | user-defined | yes |
| class (no override) | `"ClassName@hex(heapIdx)"` | no (Object intrinsic) |
| enum | enum member name (e.g. `"Red"`) | no |
| int | decimal string (e.g. `"42"`) | no |
| float | `%g` format (e.g. `"2.5"`) | no |
| string | self (identity) | no |

**Implicit coercion**: `"x" + obj` automatically calls `obj.toString()`, same
as Java/C#. This applies to class, enum, int, and float receivers. Struct
receivers are **permanently excluded** — `"x" + structInstance` is a compile
error (struct is a pure-data type in NLang; use class for object semantics).
See [Struct](struct.md).

**Enum name output**: `Color.Red.toString()` returns `"Red"` (not `"0"`). The
compiler embeds a per-enum name table; the VM uses `OP_Enum_to_str` to look
up the member name by value. Out-of-range enum values throw at runtime. See
[Enum](enum.md).

**String identity**: `"hello".toString()` returns `"hello"` — the compiler
folds this to a no-op (no opcode emitted).

**Limitations**:
- No warning when implicit coercion occurs (silent, like Java)
- `struct.toString()` / `"x" + structInstance` — permanently rejected
