# Enum


An `enum` declares a set of named integer constants. Enum values are `int32`
at runtime.

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

Members can be explicit or auto-incremented.

### Value semantics

An enum is a **value type** backed by int32:

- **Assignment / passing / return**: the int32 value is copied. Two variables
  hold independent values; mutating one never affects the other.
- **Comparison**: compared by integer value — `==`/`!=` and the relational
  operators all compare the underlying int32 (see [Operators](operators.md)).
- **No object identity**: an enum value has no heap identity. `Color.Red` is
  the int32 value `1` (or whatever it was assigned); there is no object to
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
  is rejected). Access modifiers follow class-method rules.
- Arrays of enums: methods cannot be called on the array itself — index an
  element first (`a[i].rank()`, not `a.rank()`). See [Array](array.md).

### Output

`Color.Red.toString()` returns `"Red"` (not `"0"`). The compiler embeds a
per-enum name table; the VM uses `OP_Enum_to_str` to look up the member name
by value. Out-of-range enum values throw at runtime. See
[Type Casts](type-casts.md) for the full `toString` protocol.
