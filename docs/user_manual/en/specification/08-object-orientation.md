# 8. Object Orientation

## 8.1 Classes and Inheritance

The syntax of a class declaration is §2.4. A class is a reference
type (§4.7). A class without an explicit base implicitly extends
`Object` (§4.9); the base of a class is given by `: Base`, and
inheritance is single: one base class, no multiple class
inheritance. A class may implement any number of interfaces,
`implements I1, I2` (§8.3).

Members are fields and methods. A member with no written access
modifier is `private`: class members default to private, interface
members default to public (§8.3). The access modifiers govern the
members of a class:

- `public` — reachable wherever the class itself is visible;
- `private` — reachable only inside the methods of the declaring
  class;
- `protected` — reachable inside the declaring class and inside
  the bodies of its subclasses, by bare name or through the base
  type name; nowhere else.

The same rules hold across module boundaries: of an imported class
only its `public` members are reachable, except `protected`
members inside a subclass body. A constructor that is not
`public` cannot be called from outside the class.

A field may carry an initializer (§2.4). The initializer must be a
constant expression: a literal of any type with unary negation
allowed; it may not reference a field or `this`. The default is
injected into every constructor of the class before the body runs,
base class first: a constructor that calls `super(...)` receives
the injection just after that call, one without `super(...)` at
the top of its body; with a field default present, `super(...)`
must be the first statement of the constructor. A class with no
declared constructor gets a synthesized `public` zero-argument
constructor carrying the defaults. A `native` or body-less
constructor cannot carry field defaults.

A constructor is a member declared with no return type (§2.4),
named after the class. Constructors are optional and may overload
(§7.3). A constructor call `C(args)` allocates the object and runs
the selected constructor (§5.8); `C()` for a class with no declared
constructor allocates and zero-initializes the fields. Ancestors'
constructors are not invoked automatically: without an explicit
`super(...)`, inherited fields keep their default values (§4).
`super(args);` inside a constructor invokes the direct parent's
constructor on the same object; it is valid only in a constructor
of a class with a parent, takes positional arguments only (§7.2),
and its argument list must match one parent constructor. `super()`
against a parent with no constructor is a legal no-op; arguments
in that case are a compile-time error.

Inside a method or constructor, a bare identifier that denotes a
field of the enclosing class — including an inherited field — is an
implicit `this.field` access; a local or parameter of the same name
shadows the field (§3.7). A `static` method belongs to the class
rather than to an instance: it is called through the type name,
has no `this`, and a bare call to it inside the class denotes it
directly. A static method and an instance member may share a name
when their parameter lists differ; an exact same-signature
collision is a compile-time error. Statics take no part in virtual
dispatch (§8.2); a subclass inherits its parent's statics.

## 8.2 Virtual Dispatch

The `virtual` flag marks a method for dynamic dispatch. A call
through a class reference selects the implementation by walking the
class hierarchy from the object's runtime class upwards: the
most-derived implementation of the name wins. A call to a
non-virtual method binds to the statically declared type and never
dispatches dynamically: a subclass method that shares a
non-virtual ancestor method's name shadows it for calls through the
subclass type, while a call through the ancestor type still runs
the ancestor implementation.

A subclass method that shares an ancestor virtual method's name
must match its signature exactly: the same parameter types and
count, the same return type, and the same `out` modifiers. A
mismatch is a compile-time error. Once a name is bound to a virtual
contract — an inherited virtual method or an implemented interface
method (§8.3) — the name must not be overloaded in the class or any
subclass: the same-name methods must map one-to-one onto the
contract. Within one class, a name with a `virtual` member cannot
be overloaded at all; make the whole family non-virtual to overload
it. The `Object` protocol methods are virtual and follow the same
rule (§4.9): `int equals(Object other);`, `int getHashCode();` and
`string toString();`. The default `equals` and `getHashCode`
compare object identity; `equals` yields `int` 0 or 1, not `bool`.

Overriding requires no keyword: a same-name, same-signature method
in a subclass replaces the inherited implementation. A subclass may
call the parent implementation with `super(...)` only for
constructors; there is no `super.method(...)` form.

## 8.3 Interfaces

The syntax of an interface declaration is §2.4. An interface
declares instance method signatures and carries no instance fields
and no instance implementation. An interface type is a reference
type (§4.8).

An interface instance member is a public abstract signature: the
access modifier defaults to `public`, and spelling `public` is
redundant but legal; `private`, `protected`, `const`, `native` and
`virtual` are compile-time errors on an instance member, and a
bodied instance member is a compile-time error. An interface may
declare `public static` members: static methods carry bodies and
are called through the interface name, and static constants have
the form `static const T Name = e;` with `e` a compile-time
constant; both belong to the interface type, not to any
implementing class, and take no part in the implementation
contract. A static method and an instance method of the interface
may share a name when their parameter lists differ; an exact
collision is a compile-time error, and a constant may not share a
method's name.

A class declares conformance with `implements I` (§2.4); a list of
interfaces may follow, and the `:` base form does not name an
interface. The compiler validates the contract at the class
declaration: every instance method of the interface must be
implemented by the same-name method that dispatch would select on
the class's inheritance chain, with an exactly matching signature
(§8.2). Any access level may implement a method, and a non-virtual
method or a parent's method counts as the implementation. A
missing or mismatched implementation is a compile-time error.

A value of interface type refers to an implementing object; the
conversion from the class to the interface type is implicit (§4.8,
§4.12). An interface reference may be `null`; calling through a
`null` interface reference is a catchable run-time error (§4.7,
chapter 9). A call through an interface reference dispatches
dynamically against the object's runtime class, by the same
mechanism as class virtual dispatch (§8.2).

## 8.4 Enumerations

The type face of an enumeration — its values, conversions and
default value — is §4.5. The member face is this section.

The syntax of an enumeration declaration is §2.4. Methods follow
the member list, separated from it by `;`; a trailing `;` with no
methods is accepted. Every method must have a body.

Inside an enumeration method, `this` is the underlying `int32`
value of the receiver: it participates in arithmetic and in
`switch` directly (§5.2, §6.3). A method is called through a
receiver expression — a variable, a member constant or any
enumeration-valued expression; a receiver-less call is a
compile-time error. Methods are dispatched statically: enumerations
have no inheritance and no virtual dispatch. Enumeration methods
take no default arguments and no `out` parameters (§7.2). The
built-in `toString` is reserved: a user method cannot shadow it
(§4.5). Inside a method, `this.M` for a member `M` denotes the
constant value of `M`, not a comparison with the receiver. A member
name and a method name may not collide. Methods default to
`public`; `private` and `protected` follow the visibility rules of
§8.1.

## 8.5 Structs

The value face of a struct — deep-copy semantics, defaults and
aggregates — is §4.6. The member face is this section.

The syntax of a struct declaration is §2.4. A struct holds fields
and constructors and has no methods. Struct members are always
public: fields and constructors default to `public`, and spelling
`private` or `protected` on a struct member is a compile-time
error.

A struct field may carry a constant initializer, under the same
expression rule as a class field (§8.1). Unlike a class, the
default is applied at every allocation rather than injected into a
constructor: a bare declaration, an array element, a nested struct
field and a partial initializer list all materialize the defaults,
and an initializer entry stores over them; a copy copies the stored
values and does not re-apply them (§4.6).

A struct constructor is declared with the syntax of a class
constructor (§8.1) and may overload (§7.3). A constructor runs only
on explicit construction — a constructor call or a direct-init
declaration; a bare declaration and an initializer list do not run
it, and their fields take their defaults (§4.6). Struct
constructor calls take positional arguments only (§7.2).
