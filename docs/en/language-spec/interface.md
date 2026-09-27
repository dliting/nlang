# Interface


An `interface` declares a set of method signatures with no fields and no
implementation. A class declares conformance with the `implements` keyword and
provides the implementations. An interface-typed value is a **reference** to
the implementing object (reference semantics, like a class — see
[Type Semantics](type-semantics.md)).

```nlang
interface IShape {
    public int Area();
    public int Perimeter();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
    public int Perimeter() { return 4 * this.side; }
}

int TotalArea(IShape s) {
    return s.Area();   // virtual dispatch through the interface
}
```

### What an interface supports

- **Method signatures only** — no fields, no implementation.
- **`class X implements IShape`** — a class declares conformance with the
  `implements` keyword. The `:` form is for class inheritance, **not** interface
  conformance; `class X : IShape` is rejected.
- **Virtual dispatch on interface-typed locals/params/fields** — calling a
  method through an interface reference dispatches to the implementing object's
  override (same mechanism as class virtual dispatch; see
  [Class](class.md) "Virtual methods and overrides").
- **Polymorphic collections** — a `List<IShape>` may hold a mix of
  `Square`/`Circle` objects. See
  [Built-in Generic Classes](builtin-generic-classes.md).

### The `public` keyword is required

Interface methods are declared `public int m();`. NLang's default access
modifier is `private`; an interface method declared as `int m();` (no `public`)
is parsed but treated as private and **not accessible** from call sites. The
resulting error message — "The function X does not exist or is not accessible"
— is misleading because the method does exist, just isn't public. Always write
`public int m();` in interfaces.
