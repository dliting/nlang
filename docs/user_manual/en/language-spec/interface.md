# Interface


An `interface` declares a set of method signatures with no fields and no
implementation. A class declares conformance with the `implements` keyword and
provides the implementations. An interface-typed value is a **reference** to
the implementing object (reference semantics, like a class — see
[Type Semantics](type-semantics.md)).

```nlang
interface IShape {
    int Area();
    int Perimeter();
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

### Creating interface values

```nlang
Square sq = Square();
IShape a = sq;         // upcast: the interface reference points at the instance
IShape b = Square();   // a constructor call's result assigned to an interface variable
IShape n = null;       // an interface reference can be null
```

An interface value has no standalone instance — it always points at an
implementing class's object. Upcast assignment (from an instance
variable or a constructor call) needs no cast syntax. `n == null` tests
for null directly; accessing a member on a null interface reference
(`n.Area()`) raises a `null reference error` with a backtrace at run
time.

### What an interface supports

- **Method signatures only** — no fields, no implementation.
- **`class X implements IShape`** — a class declares conformance with the
  `implements` keyword. The `:` form is for class inheritance, **not** interface
  conformance; `class X : IShape` is rejected.
- **Virtual dispatch on interface-typed locals/params/fields** — calling a
  method through an interface reference dispatches to the implementing object's
  override (same mechanism as class virtual dispatch; see
  [Class](class.md) "Virtual methods and overrides").
- **Polymorphic collections** — `List<IShape>` holds a mix of
  implementing objects such as `Square` and `Circle`; iterating with
  `foreach` and calling through the interface reference dispatches
  virtually to each implementation (`Dict` accepts interface type
  arguments in both the key and the value position). See [Built-in
  Generic Classes](builtin-generic-classes.md).

### Implementation contract

After a class declares `implements`, the compiler validates implementation
completeness at the class declaration: every method of the interface must be
implemented by the **dispatch-order-first** same-name method on the
inheritance chain with an **exactly matching signature** (the same parameter
types and count and return type — the `out` modifier must match too; any
access level works, and non-virtual methods and implementations on the parent
chain all count). A missing or mismatched implementation is a compile error,
with a message like `The class "F" does not implement the method
"val(Int32): Int32" required by interface "IF".`. A same-name method must not
coexist as an overload of an interface contract (virtual dispatch is by name;
an overload would steal the dispatch).

### Member modifiers

An interface member is exactly a **public abstract signature**. The default
access modifier is `public`: `int m();` and `public int m();` are
equivalent — the latter is redundant but legal — the opposite of
class/struct/enum members, which default to `private`. Every other modifier
on an interface member is a compile error: `private`/`protected` (with a
message like `The interface member "m" must be public.`), `const`
(interfaces declare no data to freeze), `static` (interfaces declare
instance signatures only), `native` (signatures carry no implementation),
and `virtual` (interface methods already dispatch by name). The
implementing class's matching method may carry any access level (see the
implementation contract above).
