# 4.9 Interfaces

## 4.9.1 Declaration and implementation contract

An interface is a reference type declared with `interface` (§2.4).
It declares instance method signatures and constant declarations;
it carries no instance fields and no instance implementation.

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
```

A class declares conformance with `implements I` (§2.4); a list of
interfaces may follow, and the `:` base form does not name an
interface. The compiler validates the contract at the class
declaration: every instance method of the interface must be
implemented by the same-name method that dispatch would select on
the class's inheritance chain, with an exactly matching signature
(§4.8.9). Any access level may implement a method, and a
non-virtual method or a parent's method counts as the
implementation. A missing or mismatched implementation is a
compile-time error.

*Informative:* the diagnostic names the gap, for instance
`The class "F" does not implement the method "val(Int32): Int32"
required by interface "IF".`. A same-name method must not coexist
as an overload of an interface contract, because an overload would
steal the name-based dispatch.

## 4.9.2 Interface members

An interface instance member is a public abstract signature: the
access modifier defaults to `public`, and spelling `public` is
redundant but legal; `private`, `protected`, `const`, `native` and
`virtual` are compile-time errors on an instance member, and a
bodied instance member is a compile-time error. The implementing
class's matching method may carry any access level (§4.9.1).

*Informative:* the diagnostics name the offending member, for
instance `The interface member "m" must be public.` and
`The interface instance method "m" cannot have a body`.

## 4.9.3 Static members

An interface may declare `public static` members: static methods
carry bodies and are called through the interface name, and static
constants have the form `static const T Name = e;` with `e` a
compile-time constant; both belong to the interface type, not to
any implementing class, and take no part in the implementation
contract. A static method and an instance method of the interface
may share a name when their parameter lists differ; an exact
collision is a compile-time error, and a constant may not share a
method's name.

```nlang
interface IMath {
    static int twice(int x) { return x * 2; }
    static int quad(int x) { return twice(twice(x)); }  // bare call inside the interface
    static const int base = 8;
    int sign(int x);                                    // instance signature beside statics
}
```

A static method must carry a body; `this` is not available in it.
Call it through the interface name (`IMath.twice(21)`) from any
scope where the interface is visible, including imported modules.
A static constant's value must fold to a compile-time scalar or
string constant — the same rules as a default parameter value
(§7.2) — and must be compatible with its declared type; it
is read through the interface name (`IMath.base`), and assigning
to it is a compile-time error.

## 4.9.4 Values and dispatch

A value of an interface type is a reference to an instance of a
class that implements the interface, or `null`. The default value
is `null`. An interface value has no standalone instance: it
always denotes an implementing object, and the conversion from the
class to the interface type is an implicit upcast (§4.4).

```nlang
Square sq = Square();
IShape a = sq;         // upcast: the interface reference points at the instance
IShape b = Square();   // a constructor call's result assigned to an interface variable
IShape n = null;       // an interface reference can be null
```

A call through an interface reference dispatches dynamically
against the object's runtime class, by the same mechanism as class
virtual dispatch (§4.8.9). It is a catchable run-time error if a
method is called through a `null` interface reference (§4.8.10,
chapter 8).

An interface type stands apart from every other type: it accepts
only references from classes that implement it and the `null`
literal, and its values convert implicitly to nothing — not to
`Object` and not to another interface (§4.4).

*Informative:* an interface type argument makes a polymorphic
collection: `List<IShape>` holds a mix of implementing objects,
and each call through the stored reference dispatches to its own
implementation (§4.12.6).
