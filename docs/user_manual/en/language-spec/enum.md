# Enum


An `enum` declares a set of named integer constants. Enum values are `int32`
at runtime.

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

Members can be explicit or auto-incremented (starting from 0).

### Creating enum values

```nlang
Color c = Color.Green;     // qualified member name
Color d = 1;               // int converts to enum implicitly
int n = c;                 // enum yields its underlying value, here 1
Color e;                   // no initializer: underlying value 0
```

- Declare with the **qualified member name** `Color.Green`.
- int-to-enum is allowed implicitly too (`Color d = 1;`); the explicit
  form is `1 as Color`. Whether the underlying value lands on a member
  is **not checked at compile time** — an out-of-range value only raises
  a run-time error when `toString()` looks up the name (see Output
  below).
- enum-to-int yields the underlying value implicitly (see
  [Type Semantics](type-semantics.md)).
- A declaration without an initializer (`Color e;`) gets the underlying
  value 0 — for an auto-incremented enum that is exactly the first
  member; an enum with explicitly assigned non-zero values may default
  to 0, which lands on no member (see the default value table in
  [Declarations](declarations.md)).
- Printing an enum variable requires `.toString()` first — `io.print`
  does not accept an enum variable argument directly; a member constant
  is the exception: `io.print(Color.Red)` prints the member name `Red`
  (see the printing rules in [Standard Library](standard-library.md)).

### Value semantics

An enum is a **value type** backed by int32:

- **Assignment / passing / return**: the int32 value is copied. Two variables
  hold independent values; mutating one never affects the other.
- **Comparison**: compared by integer value — `==`/`!=` and the relational
  operators all compare the underlying int32 (see [Operators](operators.md)).
- **No object identity**: an enum value has no heap identity. `Color.Red` is
  the int32 value `0` (or whatever it was assigned); there is no object to
  take a hash of or compare by reference. `switch` over an enum matches on the
  int value (see [Switch](switch.md)).
- **In aggregates**: a struct/class field of enum type is stored as int32
  (see [Struct](struct.md), [Class](class.md)).

### Enum methods

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

Methods are declared after the member list, separated by a `;`. A lone
trailing `;` with no methods (`enum E { A; }`) is accepted Java-style closing
syntax. Every method must have a body (abstract methods are rejected).

- **`this` is the enum value.** Inside a method, `this` is the int32 value of
  the receiver: it participates in arithmetic and `switch` directly
  (`this * base`, `switch (this)`), with no boxing. Methods are statically
  dispatched (`OP_CallMethodDirect`) — enums have no inheritance and no
  virtual dispatch.
- **Call through a receiver.** `c.weight(3)`, `this.weight(3)`,
  `Color.Red.weight(3)` — any enum-valued expression works. A bare `weight(3)`
  (receiver-less) is a compile error, matching class methods.
- **Parameters:** no default values (`int f(int a, int b = 5)` is rejected)
  and no `out` parameters.
- **`toString` is reserved.** The built-in `toString()` intrinsic (value →
  name) cannot be shadowed by a user method.
- **`this.<member>` resolves as a constant.** `this.Red` inside a method reads
  the member `Red`'s value — it does not compare against the receiver.
  Convenient, but easy to misread; name receivers explicitly when in doubt.
- **Member/method name sharing is a conflict** (`enum E { f; int f() {...} }`
  is rejected). Methods default to `public` — they are part of the enum's
  value surface; a spelled-out `private` or `protected` follows the same
  visibility rules as class methods (calling a non-`public` method from
  outside reports `The function "..." does not exist or is not accessible.`).
- Arrays of enums: methods cannot be called on the array itself — index an
  element first (`a[i].rank()`, not `a.rank()`). See [Array](array.md).

### Output

`Color.Red.toString()` returns `"Red"` (not `"0"`). The compiler embeds a
per-enum name table; the VM uses `OP_Enum_to_str` to look up the member name
by value. Out-of-range enum values throw at runtime. See
[Type Casts](type-casts.md) for the full `toString` protocol.
