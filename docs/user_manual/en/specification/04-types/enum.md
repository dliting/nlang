# 4.6 Enumerations

## 4.6.1 Declaration and values

An enumeration is a distinct type declared with `enum` (§2.4). It
has a fixed set of named *members*, each with a non-negative
integer value: an explicit value, or one greater than the previous
member, counting from 0. A negative explicit value is a
compile-time error.

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

The representation of an enumeration is `int`: a value of the type
is any `int`, not only a declared member. The default value is 0.
An enumeration converts implicitly to and from `int` (§4.4), and
enters `Object` boxed by its integer value (§4.11).

```nlang
Color c = Color.Green;     // qualified member name
Color d = 1;               // int converts to enum implicitly
int n = c;                 // enum yields its underlying value, here 1
Color e;                   // no initializer: underlying value 0
```

Whether an `int` converted to an enumeration lands on a declared
member is not checked at compile time: `Color d = 1;` and the
explicit `1 as Color` are accepted whatever the member table
holds, and §4.6.3 says what `toString()` reports for a value no
member carries.

An enumeration is a value type: assignment, parameter passing and
return copy the underlying integer value, so two variables then
hold independent values. Values compare by their integer value —
`==`, `!=` and the relational operators all compare the
representation — and an enumeration has no object identity:
`switch` over an enumeration matches on the integer value (§6.3).
A struct or class field of enumeration type stores the integer
value (§4.7, §4.8).

## 4.6.2 Enum methods

An enumeration may declare methods. Methods follow the member
list, separated from it by `;`; a trailing `;` with no methods is
accepted. Every method must have a body.

```nlang
enum Color {
    Red = 1, Green = 2, Blue = 4;

    public int weight(int base) {
        return this * base;
    }

    public int isPrimary() {
        switch (this) {
            case Color.Red, Color.Green, Color.Blue: return 1;
        }
        return 0;
    }
}
```

Inside an enumeration method, `this` is the underlying `int32`
value of the receiver: it participates in arithmetic and in
`switch` directly (§5.2, §6.3). A method is called through a
receiver expression — a variable, a member constant or any
enumeration-valued expression; a receiver-less call is a
compile-time error. Methods are dispatched statically:
enumerations have no inheritance and no virtual dispatch.
Enumeration methods take no default arguments and no `out`
parameters (§7.2). The built-in `toString` is reserved: a user
method cannot shadow it (§4.6.3). Inside a method, `this.M` for a
member `M` denotes the constant value of `M`, not a comparison
with the receiver. A member name and a method name may not
collide. Methods default to `public`; `private` and `protected`
follow the visibility rules of §4.8.2.

*Informative:* `this.M` reading as a constant is convenient but
easy to misread; naming the receiver explicitly removes the doubt.
A method call on an array of enumerations indexes an element
first: `a[i].weight(3)`, not `a.weight(3)` (§4.10).

## 4.6.3 toString and output

`toString()` on an enumeration value returns the name of the
member carrying that value. A value inside the member table that
no member carries returns the empty string; a value beyond the
table raises a host-level run-time error at `toString()`
(chapter 8 distinguishes host-level errors from exceptions).

```nlang
string s = Color.Red.toString();   // "Red"
```

*Informative:* the compiler embeds a per-enumeration name table
and the run-time system looks the member name up by value.
Printing an enumeration variable goes through `.toString()`:
passing an enumeration variable where text is expected is a
compile-time error, while a member constant prints its name
directly.

## 4.6.4 Enums across modules

An enumeration is a full module member: importing the package
makes its type, values, member methods and `toString` reachable
through the package name ([Packages](../03-program-structure/packages.md)).

```nlang lib.n
enum Color {
    Red, Green, Blue;

    public int rank() { return this * 10; }
}
```

```nlang
import lib;

lib.Color c = lib.Color.Green;          // qualified type + qualified value
int r = c.rank();                       // method through a variable receiver
int r2 = lib.Color.Blue.rank();         // method through a member constant
string s = lib.Color.Red.toString();    // built-in toString -> "Red"
```

`lib.Color` names the imported enumeration type in any type
position — locals, fields, parameters and return types. A
qualified member constant such as `lib.Color.Red` is a
compile-time constant that folds to its underlying value, exactly
like the unqualified `Color.Red`. Member methods work through
either receiver form with the same static dispatch as inside the
defining module, and `toString` returns the member name.
Visibility follows the same rule as everywhere: a `private`
member method is not reachable across the module boundary
(§4.8.2).
