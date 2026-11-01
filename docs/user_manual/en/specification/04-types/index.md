# 4. Types

## 4.1 Type Categories and Identity

Every expression has exactly one type. The types are:

- the primitive types: the twelve scalar types of §4.2 and
  `string` (§4.3);
- array types (§4.10);
- enumeration types (§4.6);
- struct types (§4.7);
- class types (§4.8);
- interface types (§4.9);
- the class `Object` (§4.11);
- generic class instantiations, the built-in containers `List`
  and `Dict` among them (§4.12);
- function types (§7.4).

Types fall into two value categories. The *value types* are the
scalar primitives, enumeration types and struct types: a variable
of a value type holds the value itself, and assignment copies it.
The *reference types* are class, interface and `Object` types,
array types, generic class instantiations and function types:
a variable of a reference type holds a reference to one object,
and assignment copies the reference, so source and target then
denote the same object. Every reference type accepts `null`, the
reference to no object (§4.8).

`string` is a primitive with immutable value semantics: its value
is a handle to the text's bytes, and assignment copies the handle,
but immutability makes the sharing unobservable (§4.3).

Two types are *the same type* when:

- they are the same primitive type;
- they are array types with the same element type and the same
  rank;
- they name the same type declaration — two declarations of one
  name in different bare pools (§3.7) are different types;
- they are instantiations of one generic class whose type
  arguments are pairwise the same type.

A type alias (§3.4) is another name for a type, not a type of its
own: every rule in this chapter applies to the aliased type.

### Type inventory

*Informative:* NLang is statically typed — every variable, field,
parameter and return value has a declared type, and the compiler
checks each expression against it. The table maps every type in
the language to the page that explains it in detail: how to
declare the type, its runtime semantics, and how it behaves in
expressions and statements.

| Type        | Category                    | Page |
|-------------|-----------------------------|------|
| `byte` `ubyte` `short` `ushort` `int` `uint` `long` `ulong` | primitive — integer family (value type) | [Primitives](primitives.md) |
| `float` `double` | primitive — floating point (value type) | [Primitives](primitives.md) |
| `bool`      | primitive — truth value (value type) | [Primitives](primitives.md) |
| `char`      | primitive — Unicode scalar value (value type) | [Primitives](primitives.md) |
| `string`    | primitive — immutable object  | [String](string.md) |
| `enum`      | composite — value (int32)     | [Enum](enum.md) |
| `struct`    | composite — value (deep copy) | [Struct](struct.md) |
| `class`     | composite — reference         | [Class](class.md) |
| `interface` | composite — reference         | [Interface](interface.md) |
| `T[]`       | composite — reference (heap)  | [Array](array.md) |
| `List<T>`   | built-in generic — reference  | [Generics](generics.md) |
| `Dict<K,V>` | built-in generic — reference  | [Generics](generics.md) |
| `Object`    | reference — boxed             | [Object and Boxing](object.md) |
| `func<...>` | first-class function value    | [Function values and delegates](../07-functions/function-values.md) |

### Where to look

- **What a type is, and how to declare it** — the per-type pages
  in the table above.
- **How each type behaves on assignment, parameter passing, return,
  and as a field or array element** — the summary table below.
- **How values convert between types** (implicit widening, explicit
  `as`, coercion to string) — §4.4 and [Type Casts](casts.md).
- **Declaring non-type things** (import, variables, type aliases) —
  [Declarations](../03-program-structure/declarations.md).

### Summary table

*Informative:* every type has defined semantics for four
operations — **assignment** (`a = b`), **parameter passing**,
**return value**, and use as a **field or array element**. The
table is the complete per-type summary; each row links to the
page that explains that type in detail.

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
| `func`      | reference          | copy reference   | pass reference    | return reference    | store reference     |

### Per-type notes

- **The 12 scalar primitives** (integer family, `float`/`double`, `bool`,
  `char`) — value types. Assigned, passed, and returned by value; the
  numeric family participates in arithmetic promotion and the conversion
  matrix (§4.4). See [Primitives](primitives.md).
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
  (a reference is the object's index on the heap); both names point at the same object. A null reference throws
  `NullPointerException` on member access. See [Class](class.md).
- **`interface`** — reference type like `class`; a value of interface type
  refers to the implementing object. See [Interface](interface.md).
- **`T[]`** — reference type: the array lives on the heap; assignment and
  passing copy the array *reference*, not the elements. See [Array](array.md).
- **`List<T>` / `Dict<K,V>`** — reference types; assignment and passing copy
  the container reference. See [Generics](generics.md).
- **`Object`** — reference type that may hold a boxed primitive or a class
  reference; assignment copies the reference. See [Object and Boxing](object.md).
- **`func`** — reference type: a first-class function value is a heap
  handle; assignment copies the reference, and `==` compares which
  function the handle denotes; calling a null function value raises the
  null-reference error. See [Functions](../07-functions/index.md).

### Why the two families differ

The split is value vs. reference:

- **Value types** (the 12 scalar primitives, `enum`, `string`, `struct`)
  are copied on assignment. Two variables hold independent data;
  mutating one never affects the other. (`string` is a value type even
  though it is an object, because the object is immutable.)
- **Reference types** (`class`, `interface`, array, `List`, `Dict`, `Object`,
  `func`) share the underlying object. Assignment copies the reference, so
  both names observe the same object and the same mutations.

The reference family shares one rule: **a null reference throws
`NullPointerException`** — on member access (class, interface, array,
and container nulls all behave the same) and on calling a null function
value. See [Class](class.md) for the null
semantics.

## 4.4 Assignment Compatibility and Implicit Conversion

A value is *assignable* to a type when one of the following
rules applies. The relation governs variable initialization,
assignment, argument passing and return-value conversion;
§15.1 restricts argument passing at package function calls.

- The value's type is the same type (§4.1).
- The value's type is a class and the target is that class, an
  ancestor class, or an interface the class implements (an
  implicit upcast, §4.8 and §4.9).
- The value is a scalar primitive, `string` or enumeration and
  the target is `Object` (boxing, §4.11).
- The target is `string` and the value is a scalar primitive, an
  enumeration, a class, an array or a function; the conversion
  renders the value as text, and chapter 5 fixes the rendering.
- The value is an integer type and the target's value domain
  contains the source's: widening along one sign
  (`byte`→`short`→`int`→`long`, `ubyte`→`ushort`→`uint`→`ulong`)
  or cross-sign containment (`ubyte` to `short`, `int` or `long`;
  `ushort` to `int` or `long`; `uint` to `long`).
- The value is an integer type and the target is `float` or
  `double`; the conversion MAY warn when the pair can lose
  precision.
- The value is `float` and the target is `double`.
- The value is an enumeration and the target is `int`, or the
  value is `int` and the target is an enumeration (§4.6).
- The value is the `null` literal and the target is a reference
  type or `string`. No other integer converts implicitly to a
  reference type.
- The value is an integer literal whose value fits the target
  integer type's value domain, even when the general conversion
  between the two types is explicit-only; a literal that does not
  fit is a compile-time error. Only literals take this rule: an
  expression of the source type does not, even when it folds to a
  constant.
- The value is a `double` literal exactly representable as
  `float` and the target is `float`; a floating-point literal is
  never implicitly assignable to an integer type.

Every other conversion is rejected in implicit flow: it is a
compile-time error. In particular:

- narrowing integers and cross-sign pairs outside the
  containment list above;
- `float` or `double` to any integer;
- `char` to or from any numeric type;
- `bool` to any type but `string` and `Object`, and any type to
  `bool`;
- `string` to any type but `string` and `Object`;
- `Object` to a class (a downcast) and `Object` to a scalar
  primitive or `string` (unboxing);
- a struct to any other type, including `Object` and `string`;
- an interface value to any other type;
- an array to any other array type or to `Object`;
- a function value to `Object`;
- one generic instantiation to a different one.

The `as` operator performs the explicit conversions — narrowing,
`char` conversions, downcasts and unboxing — and §4.5 and §5.5
define it.

### Numeric conversion matrix

*Informative:* the tiers above, arranged by conversion direction.
Conversions between numeric types come in three tiers: **implicit**
(the compiler inserts them), **implicit with a lossy warning**, and
**explicit `as`** (user-acknowledged, no warning). Examples are on
[Primitives](primitives.md) and [Type Casts](casts.md).

**Implicit (no warning)**:

- Same-sign rank widening: `byte→short→int→long`;
  `ubyte→ushort→uint→ulong`.
- Cross-sign range containment: `ubyte→short/int/long`;
  `ushort→int/long`; `uint→long`.
- Integer→float where the target represents the source range exactly:
  narrow integers (byte..ushort) → `float/double`; `int/uint` →
  `double` only.
- Float widening: `float→double`.
- `char→string` (encoded as that code point's Unicode Transformation Format (UTF-8) bytes);
  `enum→int` (its backing type).

**Implicit with a lossy warning** — when the target's range cannot hold
the source's, the compiler emits `implicit conversion from 'X' to 'Y'
loses precision` (`ncc --no-warn` suppresses this warning family):

- `int/uint/long/ulong → float`
- `long/ulong → double`

Constant operands whose value is exactly representable are exempt
(`float f = 5;` warns nothing) — the warning targets "the actual loss of
this conversion", not "the possible loss of the type pair".

**Constant fit**: an integer **literal** assigned to a narrower target is
implicitly legal when the value is in the target's range
(`byte b = 5;`); it covers only the literal itself, not constant
arithmetic expressions (`byte b = 1 + 2;` still needs `as`). Double literals into a
`float` target follow the same rule. See
[Primitives](primitives.md).

**Explicit `as`**: all narrowing (`float→int`, `double→float`,
`long→int`, `ulong→long`, ...), reverse cross-sign (`short→ubyte`, ...),
and `numeric↔char`. See [Type Casts](casts.md).

**Forbidden**: `bool` to or from any numeric type — bool reaches
`string` by coercion and `Object` by boxing, and nothing else —
and `string → numeric` via `as` (use the `toInt()` family).

**Arithmetic promotion**: integer operands narrower than int are first
promoted to int; the result type is the smallest type that can implicitly
receive both operands (`int + uint` → `long`; mixing `int`/`long` with
`ulong` is a compile error); when floating point participates, the larger
float rank wins. bool and char take no part in arithmetic. See
[Operators](../05-expressions/operators.md).
