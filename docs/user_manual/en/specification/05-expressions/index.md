# 5. Expressions

An expression produces a value. NLang is statically typed: every expression
has a type, and the compiler checks each operation against it.

| Expression                                     | Page |
|------------------------------------------------|------|
| operators (`+ - * / %`, comparison, logical)   | [Operators](operators.md) |
| casts (`as`, coercion to string)               | [Type Casts](../04-types/casts.md) |
| collection initializers (`[...]`, `Type{...}`) | [Collection Initializers](initializers.md) |

String interpolation (`${...}`) is §5.4; escape sequences and string
comparison are string-literal features: see
[String](../04-types/string.md). Char literals (`'a'`,
`'A'`) and type semantics are on [Primitive Types](../04-types/primitives.md).

## 5.1 Evaluation Order

The grammar of expressions is §2.6, and §2.7 fixes the operator
precedence and associativity. The subexpressions of an expression
are evaluated left to right, in the order the grammar presents
them: the left operand of an operator before the right operand,
the receiver of a member access before the arguments of a call,
the arguments of a call in order, and the indices of a subscript
in order. Each subexpression is fully evaluated — including its
side effects — before the next begins; an implementation MUST NOT
reorder or elide subexpressions that have observable side
effects.

The logical operators `&&` and `||` are left to right and
short-circuit: the right operand is evaluated only when the left
operand does not already decide the result — for `&&` when the
left operand is `true`, for `||` when it is `false`. The operands
of `&&`, `||` and `!` are `bool` (§4.2): no other type is
accepted.

## 5.2 Arithmetic and Relational Operators

The binary operators `+ - * / % << >> < <= > >= == != && ||` and
the unary operators `- + !` are fixed by §2.7. This section fixes
their operand types and results.

`bool` and `char` do not enter arithmetic (§4.2). An
enumeration participates in arithmetic as `int` (§4.6). For a
binary numeric operator the two operands take a *common type*:

- if either operand is floating-point: `double` when either
  operand is `double`, otherwise `float`;
- otherwise both operands are integers: each operand narrower
  than `int` first becomes `int`; if the two are then the same
  type, that is the common type; otherwise the common type is the
  smallest integer type whose value domain contains both, and
  when no integer type contains both — as for `int` with `ulong`
  — the operation is a compile-time error.

Both operands are converted to the common type (§4.4), and the
result has the common type.

`+ - * /` and `%` are defined for every numeric type. Integer
arithmetic wraps around modulo the width of the result type
(§4.2). Integer `/` truncates the quotient toward zero and `%`
takes the sign of the dividend: `(−7)/2` is `−3`, `(−7)%3` is
`−1`, and `7%(−3)` is `1`. Floating-point arithmetic follows
IEEE 754 at the operand width; `NaN` is not equal to itself.
Floating-point `%` is the remainder with the sign of the
dividend: `(−7.5)%2.0` is `−1.5`.

It is a run-time error to divide or to take the remainder by
zero, for every numeric type including the floating-point types
— a floating-point division by zero yields no infinity. The
error is raised at the operator as a catchable exception
(chapter 8).

`<<` and `>>` shift an integer left operand of any integer type
(§4.2); a `byte`, `ubyte`, `short` or `ushort` left operand is
promoted to `int`, and the result has the promoted left
operand's type. The count operand must be assignable to that
type (§4.4). A count outside `0` to one less than the width of
the left operand is a run-time error raised as a catchable
exception (chapter 8); the check is a run-time check, so a
constant out-of-range count is not a compile-time error. `>>` is
an arithmetic shift: the vacated high-order bits take the value
of the sign bit, so `−8 >> 1` is `−4`.

`<`, `<=`, `>` and `>=` are defined for numeric types (after
promotion), for `char` by code point value, for `string` by the
byte order of §4.3, and for enumerations as `int`. `bool` values
cannot be ordered: an ordering of `bool` operands is a
compile-time error. `==` and `!=` are defined for numeric types
(after promotion), for `char` by code point value, for `string`
by content (§4.3), for `bool`, for enumerations by value, and for
class, interface and `Object` references by identity (§4.8, §4.9,
§4.11).

The unary `-` and `+` are defined for every numeric type, and the
result has the operand's type; `-` on an unsigned integer wraps
(§4.2). The unary `!` takes a `bool` operand (§5.1).

## 5.3 String Concatenation

The `+` operator with a `string` operand on either side is string
concatenation: the other operand is converted to `string` (§4.4)
and the two byte sequences are joined; the result is `string`.
`+` is left-associative, so `2 + 3 + "y"` is `"5y"` and
`"y" + 2 + 3` is `"y23"`.

The conversion of a value to `string` renders it as text:

- an integer: decimal digits, a leading `-` when negative, no
  leading zeros and no separators;
- `bool`: `true` or `false`;
- `char`: the single code point;
- `float` and `double`: the shortest sequence of significant
  digits that round-trips exactly at the type's width, written in
  fixed-point notation when the decimal exponent is at least −4
  and less than 16 and in exponential notation otherwise —
  `1000000`, `1.2e-06` and `1.5` render as written; `NaN`
  renders as `nan`, an infinity as `inf` or `-inf`, and zero as
  `0` or `-0`;
- an enumeration: the name of the member carrying the value
  (§4.6).

A class, array or function value renders through its `toString`
(§4.11.2).

## 5.4 String Interpolation

A string literal may contain `${name}`, where `name` is an
identifier: each occurrence stands for the string rendering of
the value that `name` denotes. The name resolves as a bare name
(chapter 3) and renders as in §5.3; an interpolated literal means
the same as the concatenation of its parts, evaluated left to
right (§5.1).

Only a bare identifier is allowed between `${` and `}`. Any other
form — an expression, a qualified name, a member access — is a
compile-time error.

## 5.5 Cast Expressions

`e as T` converts the value of `e` to the type `T`.

At compile time, `as` is defined between two types when the
conversion is one of the following:

- the same type — including an upcast to an ancestor class or to
  `Object`, which are no-ops (§4.8, §4.11), and an enumeration to
  or from `int` (§4.6);
- boxing into `Object` (§4.11);
- a downcast from a class type to one of its descendant classes;
- an unboxing from `Object` to a scalar primitive, `string` or
  enumeration;
- an explicit numeric conversion: a narrowing or sign-changing
  integer conversion, `float` or `double` to an integer, or
  `char` to or from an integer;
- a function value to `string`; the conversion applies with or
  without the operator, and §5.3 fixes the rendering.

An implicit conversion that is not one of these — widening
integers, an integer to a floating-point type, `float` to
`double`, or any type but a function value to `string` — is a
compile-time error with `as`: it applies without the operator
(§4.4). A pair with no
conversion path is a compile-time error; `bool` has no `as` path
to or from any type but `Object` (§4.2): its conversion to
`string` is implicit and takes no operator.

At run time:

- between numeric types: `float` or `double` to an integer
  truncates the fractional part toward zero, so `7.9` becomes `7`
  and `−3.7` becomes `−3`; an integer to another integer keeps
  the low bits of the target's width, so `300 as byte` is `44`;
- to `char`: the value must be a Unicode scalar value (§4.2);
  otherwise the cast raises a catchable run-time error
  (chapter 8);
- a downcast of a reference to a class type yields the same
  reference when the value is an instance of that class or of a
  subclass of it; otherwise it raises a catchable run-time error
  (chapter 8);
- an unboxing of `Object` to a scalar primitive or `string`
  reads the boxed value out when the box holds a value of the
  target type; otherwise it fails at the host level (chapter 8);
  an unboxing to an enumeration reads the boxed `int` value
  when the box holds one, failing at the host level otherwise
  (chapter 8); the value is not checked against the member table
  of §4.6;
- a downcast or an unboxing of a `null` reference fails at the
  host level (chapter 8), while an upcast of a `null` reference
  yields `null`;
- an enumeration cast is unchecked (§4.6).

## 5.6 Calls

A call `f(a1, ..., an)` evaluates the callee and the arguments
left to right (§5.1). Each argument must be assignable to its
parameter's type (§4.4). The result of a call is the callee's
return type; a call to a `void` function is used as a statement
(chapter 6). Calling a function value whose value is `null` is a
catchable run-time error (chapter 8). Default arguments, named
arguments, `out` parameters and overloading are defined in
chapter 7.

## 5.7 Member Access and Subscript

`x.m` selects the member `m` of the value of `x`; member lookup
and dispatch are defined in §4.8 and §4.9. Accessing a member or
calling a method through a `null` class or interface reference is
a catchable run-time error (§4.8, chapter 8).

An array subscript `a[i1, i2, ..., ik]` carries exactly as many
indices as the array's rank — a different count is a compile-time
error — and each index is an integer expression. An index outside
`0` to one less than the length of its dimension is a catchable
run-time error, on a read and on a write alike (chapter 8). A
subscript read yields the stored element; for an element of struct
type it yields a copy (§4.10). A subscript write stores a value
assignable to the element type (§4.4).

A string subscript `s[i]` reads the byte at byte offset `i` as a
`ubyte`; the index must be an integer expression, and an
out-of-range index is a catchable run-time error (chapter 8). A
string subscript is read-only: writing through it is a
compile-time error (§4.3). Code-point access is the `charAt`
method (§16.2).

The subscript of a `List` or `Dict` is sugar over its `get` and
`set` methods (§12): `li[i]` reads and writes the element
at position `i` with the same bounds error, and `d[k]` reads and
writes the value stored under key `k` — reading a key the map
does not hold is a catchable run-time error (§4.12, chapter 8).

### Member access

```nlang
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check — see
[Class](../04-types/class.md) "Identity, equality and null"). A struct has no general methods — only
constructors (see [Struct](../04-types/struct.md)); a field is read or written through
the member expression. Enum members are constants, not members of a value
(see [Enum](../04-types/enum.md)).

## 5.8 Initializers and Creation Expressions

An array creation `T[n]` or `T[n1, ..., nk]` gives one length per
dimension of the type (§4.10). The lengths are integer
expressions; the run-time constraints of §4.10 are checked at the
creation site.

An array literal `[e1, ..., en]` creates a rank-1 array. The
target type fixes the element type, and each element is converted
to it (§4.4); without a target type the elements must all have
one type, which becomes the element type. For a type of rank
above 1, a literal of literals fills one dimension at a time —
`int[,] m = [[1, 2], [3, 4]];` — and every row must have the same
length, because the shape of an array value never changes (§4.10).
Above the leaf level an entry may also be an expression whose
value is an array one dimension below the target; with such an
entry the shape is fixed at run time by the value of the first
entry, and a later entry of a different width raises a catchable
exception (chapter 8).

A struct brace initializer `T{...}` fills the fields of a new
struct value by name (`T{f: v, ...}`) or by position (`T{v, ...}`)
in declaration order; omitted fields keep their defaults (§4.7).
A class is created by `T(args)`: the call allocates a new instance
and runs the constructor (§4.8.4). A `List` or `Dict` is
created by a brace initializer as defined in §2.6 — positional
entries append to a `List`, `key: value` entries store into a
`Dict` — and the entries are evaluated left to right (§5.1).

### Creation and initialization quick reference

| Type | Creation forms | Details |
|---|---|---|
| class | constructor call `Node(42)`; direct-init declaration `Node n(42);`; braced anchor `P{value: 42}` | [Class](../04-types/class.md) "Instance creation" |
| struct | constructor call `Point(1, 4)`; direct-init declaration `Point p(2, 3);`; named entries `Point{x: 1, y: 2}`; positional entries `Point{3, 7}`; bare declaration zero-initializes `Point p;` | [Struct](../04-types/struct.md) "Creation and initialization" |
| enum | qualified member `Color.Red`; implicit int conversion `Color c = 1;` | [Enum](../04-types/enum.md) |
| arrays | expression position `int[3]`, `[1, 2, 3]`; declaration position `int a[3];`, `int b[100] = [7, 8];` | [Array](../04-types/array.md) |
| interface | upcast `IShape s = sq;` — an interface value always refers to an implementing object | [Interface](../04-types/interface.md) |
| List/Dict | literals `[1, 2]`, `{"k": 1}`; empty `{}` | [Collection initializers](initializers.md) |

A constructor call `Node(42)` is an expression: it allocates on the heap, runs the constructor with the arguments, and returns a reference (see [Class](../04-types/class.md) "Constructors and super()"). A struct constructor call `Point(1, 4)` is an expression too; braced entries `Point{...}` fill fields directly without running a constructor (see [Struct](../04-types/struct.md) "Creation and initialization"). Declaration-position forms (`T name(args);`, `T name[n];`, `T name[n] = [...]`) are allowed on every declarator of a multi-declarator declaration (see [Declarations](../03-program-structure/declarations.md)).

## 5.9 Function Value Expressions

A function or method name written without a call is a function
value (§7.4):

- a free function name denotes that function;
- `C.m` denotes a static method of the class `C`;
- `expr.m` denotes an instance method bound to the value of
  `expr`: the receiver is captured when the function value is
  formed;
- a bound reference to a virtual or interface method dispatches
  against the receiver's runtime class at each call (§4.8.9, §4.9.4).

The type of the value is `func<R, P1, ..., Pn>`, matching the
signature of the denoted function exactly (§7.4). A function
value is called with `v(a1, ..., an)` (§5.6).
