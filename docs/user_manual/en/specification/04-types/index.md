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
- the generic container types `List` and `Dict` (§4.12);
- function types (§4.13).

Types fall into two value categories. The *value types* are the
scalar primitives, enumeration types and struct types: a variable
of a value type holds the value itself, and assignment copies it.
The *reference types* are class, interface and `Object` types,
array types, `List` and `Dict` instantiations and function types:
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
- they are instantiations of one generic container whose type
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
| `enum`      | composite — value (int32)     | [Enum](../../language-spec/enum.md) |
| `struct`    | composite — value (deep copy) | [Struct](../../language-spec/struct.md) |
| `class`     | composite — reference         | [Class](../../language-spec/class.md) |
| `interface` | composite — reference         | [Interface](../../language-spec/interface.md) |
| `T[]`       | composite — reference (heap)  | [Array](../../language-spec/array.md) |
| `List<T>`   | built-in generic — reference  | [Built-in Generic Classes](../../language-spec/builtin-generic-classes.md) |
| `Dict<K,V>` | built-in generic — reference  | [Built-in Generic Classes](../../language-spec/builtin-generic-classes.md) |
| `Object`    | reference — boxed             | [Object & Boxing](../../language-spec/object.md) |
| `func<...>` | first-class function value    | [Function types & delegates](../../language-spec/function-types-and-delegates.md) |

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
  identity. See [Enum](../../language-spec/enum.md).
- **`struct`** — value type with **deep-copy** semantics: copying copies the
  whole aggregate, including nested struct fields. The one exception is a
  class-typed field inside the struct, which is shallow-copied (the reference
  is shared). See [Struct](../../language-spec/struct.md).
- **`class`** — reference type. Assignment/passing/return copy the reference
  (a reference is the object's index on the heap); both names point at the same object. A null reference throws
  `NullPointerException` on member access. See [Class](../../language-spec/class.md).
- **`interface`** — reference type like `class`; a value of interface type
  refers to the implementing object. See [Interface](../../language-spec/interface.md).
- **`T[]`** — reference type: the array lives on the heap; assignment and
  passing copy the array *reference*, not the elements. See [Array](../../language-spec/array.md).
- **`List<T>` / `Dict<K,V>`** — reference types; assignment and passing copy
  the container reference. See [Built-in Generic Classes](../../language-spec/builtin-generic-classes.md).
- **`Object`** — reference type that may hold a boxed primitive or a class
  reference; assignment copies the reference. See [Object & Boxing](../../language-spec/object.md).
- **`func`** — reference type: a first-class function value is a heap
  handle; assignment copies the reference, and `==` compares which
  function the handle denotes; calling a null function value raises the
  null-reference error. See [Functions](../../language-spec/functions.md).

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
value. See [Class](../../language-spec/class.md) for the null
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
[Operators](../../language-spec/operators.md).

## 4.6 Enumerations

An enumeration is a distinct type declared with `enum` (§2.4). It
has a fixed set of named *members*, each with a non-negative
integer value: an explicit value, or one greater than the previous
member, counting from 0.

The representation of an enumeration is `int`: a value of the type
is any `int`, not only a declared member. The default value is 0.
An enumeration converts implicitly to and from `int` (§4.4), and
enters `Object` boxed by its integer value (§4.11).

`toString()` on an enumeration value returns the name of the
member carrying that value. A value inside the member table that
no member carries returns the empty string; a value beyond the
table raises a host-level run-time error at `toString()`
(chapter 9 distinguishes host-level errors from exceptions).

An enumeration may declare methods; chapter 8 defines the member
surface.

## 4.7 Structs

A struct is a value type declared with `struct` (§2.4). Its
members are fields and constructors; every field of a struct is
public.

A field may declare an initializer. The default value of a struct
is each field's default: the declared initializer when present,
otherwise that type's default.

A struct may declare constructors. A constructor runs only when
the construction is explicit — `T(args)` or `T x(args)`; a bare
declaration and a brace initializer list fill fields without
running any constructor (chapter 6 and chapter 8).

Assignment between struct values copies the whole value: every
field is copied. A field of a reference type copies its reference,
so the two struct values then share that one object (chapter 10).

A brace initializer list fills fields by name or by position;
fields it omits keep their defaults (chapter 5).

## 4.8 Classes

A class is a reference type declared with `class` (§2.4). A value
of a class type is a reference to an instance of the class, or
`null`. The default value is `null`.

A class has single inheritance: at most one parent class, named in
the declaration (chapter 8). Every class is ultimately rooted at
`Object` (§4.11).

Two class references compare by identity: `==` holds exactly when
both denote the same instance or both are `null`. The method
`equals` is a separate, opt-in operation (chapter 8).

It is a run-time error if a member is accessed or a method is
called through a `null` reference; the error is raised as a
`NullPointerException` (chapter 9).

Construction allocates a new instance and runs the class's own
constructor; inherited fields take their defaults unless the
constructor chains to a parent constructor (chapter 8).

## 4.9 Interfaces

An interface is a reference type declared with `interface` (§2.4).
It declares instance method signatures and constant declarations;
chapter 8 defines the member surface.

A value of an interface type is a reference to an instance of a
class that implements the interface, or `null`. A class implements
an interface by declaring it (chapter 8); a call through an
interface value dispatches to the instance's overriding method
(chapter 8). The default value is `null`.

An interface type stands apart from every other type: it accepts
only references from classes that implement it and the `null`
literal, and its values convert implicitly to nothing — not to
`Object` and not to another interface (§4.4).

## 4.10 Arrays

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

An array value is created with a length for each dimension, and
those lengths never change for the value's lifetime. Every element
starts as the default value of the element type. It is a run-time
error if a creation length is negative or if the product of the
lengths exceeds 100000000; the error is raised at the creation
site as an `IndexOutOfBoundsException` (chapter 9).

An array value is a reference type: assignment copies the
reference, and two variables then share the same elements. Reading
an element of struct type yields a copy of the stored struct, not
the stored struct itself; writing a field of that copy does not
change the array, and storing a struct back requires an assignment
to the element (chapter 10).

Every array value supports three members: `length()` returns the
total number of elements as an `int`; `rank()` returns the rank;
`shape()` returns the length of each dimension. Subscripting and
iteration are defined in chapter 5 and chapter 6.

## 4.11 Object and Boxing

`Object` is the root class. Every class that declares no parent
implicitly derives from `Object`; naming `Object` as the parent
explicitly is a compile-time error. A value of type `Object` is a
reference to any class instance — including a `List` or `Dict`
instance — or `null`.

`Object` declares the protocol members `equals`, `getHashCode`
and `toString`; chapter 8 defines their signatures, overriding and
dispatch.

A scalar primitive, `string` or enumeration value entering an
`Object` position is *boxed*: the value is wrapped in a heap
object that the `Object` reference then denotes. Boxing is
implicit at the entry point — an initializer, an assignment or an
argument — and there are no named wrapper types. An enumeration
boxes by its integer value. Reading the value back out requires
the `as` operator (chapter 5).

No other type has a path into `Object`: array, struct, interface
and function values are not assignable to `Object` and not
convertible to it with `as` (§4.4, chapter 5).

## 4.12 Generic Containers: List and Dict

`List<T>` and `Dict<K, V>` are built-in generic classes. They are
the only generic types in the language: a user-declared class
cannot take type arguments.

Each instantiation is a distinct type (§4.1): `List<int>` and
`List<string>` are different, mutually incompatible types, and so
are `Dict<string, int>` and `Dict<string, long>`.

Both are reference types: values are references to instances, the
default is `null`, and an instance is assignable to `Object`
(§4.11).

A `List` is an ordered sequence of elements with operations to
add, read, replace, remove and search (chapter 13). A `Dict` maps
keys to values with operations to store, read, test, remove and
count (chapter 13); reading a value for a key the map does not
hold throws an `Exception` (chapter 9).

`Dict` key equality is kind-aware: primitive keys compare by
value, with `NaN` not equal to itself; `string` keys compare by
content; class, struct and array keys compare by identity.
Iteration over a `Dict` walks its keys (chapter 6).

*Informative:* the runtime representation of both containers
erases the type arguments; the static type governs compile-time
checking only.

## 4.13 Function Types

A function type is written `func<R, P1, ..., Pn>`: the first type
argument is the return type — `void` when the function returns no
value — and the remaining arguments are the parameter types in
order (§2.3). A parameter type argument may carry the `out`
prefix. A function with no parameters is written `func<R>`.

`void` appears as a type argument only in the return slot of
`func<...>`; any other type argument written `void` — such as
`List<void>` — is a compile-time error.

Two function types are the same type when their return types,
parameter types and `out` markers match position by position.
There is no variance: neither the return type nor a parameter type
may differ, even by an implicit conversion (§4.4).

A value of a function type denotes a function: a free function, a
static method, or a method bound to an instance (chapter 7). The
default value is `null`; calling a `null` function value is a
run-time error. A function value is a reference type whose only
implicit conversion is to `string` (§4.4): it does not enter
`Object`.
