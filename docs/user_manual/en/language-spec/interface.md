# Interface

The normative rules are in [specification §4.8](../specification/04-types.md).

An `interface` declares a set of instance method signatures with no
fields and no implementation. A class declares conformance with the
`implements` keyword and provides the implementations. The interface
itself may also carry `public static` members — methods with bodies and
constants — which belong to the interface type rather than to any
implementing class (see "Static members" below). An interface-typed
value is a **reference** to the implementing object (reference
semantics, like a class — see [Type Semantics](type-semantics.md)).

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
(`n.Area()`) throws a `NullPointerException` at run time, catchable
with `try/catch` — see [Exceptions](exception.md).

### What an interface supports

- **Instance method signatures only** — no instance fields, no instance
  implementation. Static methods (with bodies) and static constants
  belong to the interface itself (see "Static members" below).
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
an overload would steal the dispatch). Static members take no part in the
contract (see "Static members" below).

### Member modifiers

An interface **instance** member is exactly a **public abstract
signature**. The default access modifier is `public`: `int m();` and
`public int m();` are equivalent — the latter is redundant but legal —
the opposite of class/struct/enum members, which default to `private`.
Every other modifier on an interface instance member is a compile
error: `private`/`protected` (with a message like `The interface member
"m" must be public.`), `const` (a method cannot be const), `native`
(signatures carry no implementation), and `virtual` (interface methods
already dispatch by name). A bodied instance member is also rejected
(`The interface instance method "m" cannot have a body`). The
implementing class's matching method may carry any access level (see the
implementation contract above).

### Static members

An interface may declare `public static` members. They belong to the
interface type itself, shared by every implementing class, and take no
part in the implementation contract:

```nlang
interface IMath {
    static int twice(int x) { return x * 2; }
    static int quad(int x) { return twice(twice(x)); }  // bare call inside the interface
    static const int base = 8;
    int sign(int x);                                    // instance signature beside statics
}
```

- **Static methods** must be public and must carry a body
  (`static int f();` is rejected with `The interface member "f" is
  static but has no body`). Call them through the interface name,
  `IMath.twice(21)`, from any scope where the interface is visible —
  including imported modules. Inside a static method the bare form calls
  sibling statics directly; `this` is not available (`this is not
  available in a static method`).
- **Static constants** have the form
  `[public] static const Type Name = constant-expression;`. The value
  must fold to a compile-time scalar or string constant (the same rules
  as a default parameter value) and must be compatible with the
  declared type. Read them through the interface name, `IMath.base`;
  assigning to them is a compile error.
- **Separate namespace**: a static method and an instance method may
  share a name with different parameter lists, but an exact
  same-signature collision is a compile error. A constant may not share
  its name with a method of the interface.
