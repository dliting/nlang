# 4.8 Classes

## 4.8.1 Declaration and reference semantics

A class is a reference type declared with `class` (§2.4). A value
of a class type is a reference to an instance of the class, or
`null`. The default value is `null`. A class lives on the heap and
is reached through its reference; it carries fields, constructors
and methods, and a single inheritance line.

```nlang
class Node {
    public int value;
    public Node next;

    public Node(int v) {
        this.value = v;
    }

    public int getValue() {
        return this.value;
    }
}

class SpecialNode : Node {
    public int extra;
}
```

A class has single inheritance: at most one parent class, named in
the declaration (§4.8.5). Every class is ultimately rooted at
`Object` (§4.11).

Class values follow reference semantics throughout the language:

- **Assignment** (`obj2 = obj1`) copies the reference; both
  variables then denote the same object.
- **Parameter passing** passes the reference: the callee can modify
  the object's fields and the caller sees the changes.
- **Return value** returns the reference; no copy is made.
- **As a struct field**: the struct stores the reference, and a
  struct copy shallow-copies it (§4.7.4).

## 4.8.2 Access modifiers

A member with no written access modifier is `private`: class
members default to private, interface members default to public
(§4.9.2). The access modifiers govern the members of a class:

- `public` — reachable wherever the class itself is visible;
- `private` — reachable only inside the methods of the declaring
  class;
- `protected` — reachable inside the declaring class and inside
  the bodies of its subclasses, by bare name or through the base
  type name; nowhere else.

The same rules hold across module boundaries: of an imported class
only its `public` members are reachable, except `protected`
members inside a subclass body (§4.8.11). A constructor that is
not `public` cannot be called from outside the class.

*Informative:* the diagnostics name the offending member, for
instance `constructor of class "C" is not accessible`.

## 4.8.3 Fields and field initializers

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

```nlang
class Cfg {
    public int x = 5;
    public string s = "hi";
    public float f = -1.5;
}
```

Because the synthesized constructor carries the defaults, both
`Cfg()` and the brace form `Cfg{}` apply them.

## 4.8.4 Instance creation

A constructor call `C(args)` allocates the object and runs the
selected constructor (§5.8); `C()` for a class with no declared
constructor allocates and zero-initializes the fields. Inside the
class body the class's own name still denotes a constructor call.

```nlang
Node a = Node(42);        // constructor call: heap-allocates and runs the ctor
Node b(42);               // direct-init declaration, equivalent to the line above
Node c;                   // declaration only: c is null
```

The anchored brace form `C{field: value, ...}` fills fields by
name after construction; the class must have a parameterless
constructor, explicit or synthesized — `Node` above, whose
constructor takes `Node(int v)`, does not qualify. The
declaration-site forms work on every declarator of a
comma-separated list; the declarator shapes are §3.8.

## 4.8.5 Inheritance and instance layout

The base of a class is given by `: Base`, and inheritance is
single: one base class, no multiple class inheritance. A class may
implement any number of interfaces, `implements I1, I2` (§4.9.1).
A class without an explicit base implicitly extends `Object`
(§4.11).

*Informative:* the object's memory layout is `[typeId,
ancestor_fields..., parent_fields..., own_fields...]`. The type ID
in the first slot records the object's actual class, and virtual
dispatch selects the implementation through it at run time
(§4.8.9).

## 4.8.6 Constructors and super()

A constructor is a member declared with no return type (§2.4),
named after the class. Constructors are optional and may overload
(§7.3). Ancestors' constructors are not invoked automatically:
without an explicit `super(...)`, inherited fields keep their
default values.

`super(args);` inside a constructor invokes the direct parent's
constructor on the same object; it is valid only in a constructor
of a class with a parent, takes positional arguments only (§7.2),
and its argument list must match one parent constructor. For a
parent in the built-in Exception family, the parent constructor is
`Exception(string message, Exception cause = null)` — both
`super(msg)` and `super(msg, cause)` work (§8.3). `super()`
against a parent with no constructor is a legal no-op; arguments
in that case are a compile-time error.

```nlang
class Base {
    public int v;
    public Base(int x) { this.v = x; }
}
class Kid : Base {
    public Kid(int x) {
        super(x * 2);        // calls Base(int)
    }
}
```

*Informative:* `super(...)` may appear at any statement position of
the constructor body except when the class carries a field
default, which pins it to the first statement (§4.8.3). Named
arguments are not accepted on `super(...)`.

## 4.8.7 Name resolution and this

Inside a method or constructor, a bare identifier that denotes a
field of the enclosing class — including an inherited field — is
an implicit `this.field` access; a local or parameter of the same
name shadows the field (§3.7). The implicit form works for reads,
assignments, compound assignments and default-parameter
expressions.

## 4.8.8 Static members

A `static` method belongs to the class rather than to an instance:
it is called through the type name, has no `this`, and a bare call
to it inside the class denotes it directly. A static method and an
instance member may share a name when their parameter lists
differ; an exact same-signature collision is a compile-time error.
Statics take no part in virtual dispatch (§4.8.9); a subclass
inherits its parent's statics.

```nlang
class Util {
    public static int twice(int x) { return x * 2; }
    public static int quad(int x) { return twice(twice(x)); }  // bare call inside the class
    public int inst() { return 1; }
}
class Base {
    public static int bid() { return 7; }
}
class Sub : Base {
    public int pad;
}
int main() {
    return Util.twice(21) + Util.quad(2) + Sub.bid();  // 42 + 8 + 7
}
```

*Informative:* the two call shapes are kept apart: calling a static
through an object is rejected (`must be called through the type
name`), and calling an instance method through the type name is
rejected too (`must be called through a receiver object`). Using
`this` in a static method is a compile error (`this is not
available in a static method`).

### Static fields

A `static` field has one storage slot on the class itself instead
of one per instance: every read and write — from any method, from
any instance, and through the type name — addresses that single
slot. A static field is declared like an instance field with the
`static` flag (§2.4), and an initializer is optional; a
declaration without one starts at the type's default value.

```nlang
class Counter {
    public static int total;          // defaults to 0
    public int serial;
    public Counter() {
        Counter.total = Counter.total + 1;
        this.serial = Counter.total;  // shared state, read per instance
    }
}
```

Access through the type name (`Counter.total`) is the universal
form, visible wherever the class is. Inside the class a bare name
denotes the static field directly, like any member. Access through
an object is rejected the same way a static method call on a receiver
is. A struct has no static members at all — the
flag on a struct field is a compile-time error — and a generic
class cannot declare static fields, because its members belong to
the one erased implementation shared by every instantiation.

An inherited static field stays owned by the class that declares
it: `Sub.total` and `Base.total` name the same slot, and the
initializer — if any — belongs to the declaring class.

A static field of a struct type follows the struct value model
(§4.7.4): storing a value into the slot deep-copies the record,
so the slot owns an independent copy — a later change to the
stored source never shows through — and a member write through
the slot (`Registry.anchor.x = 5`) mutates the slot's own record.
A declaration without an initializer starts at the struct's zero
value, every field at its default, nested structs included
(§4.7.2).

### Static constants

A `static const` field adds immutability to a static field:
assignment outside the initializer is a compile-time error, and
the declaration requires an initializer. Its initializer takes one
of two channels:

- a constant-foldable expression (a literal, arithmetic over
  literals, ...) folds at compile time into the declared constant
  — no storage and no run-time code exist for it;
- any other expression initializes the slot lazily like a static
  field's initializer (below), with the same exactly-once
  guarantee, and every later read is read-only.

A `static const` of a struct type always takes the lazy channel:
a struct value cannot fold, so the initializer builds the record
once at first use, and the value semantics are the static field's
— reading the constant into a variable deep-copies it (§4.7.4).

### Lazy initialization

A static field (or non-folded static constant) with an initializer
is initialized *lazily*: nothing runs at program start, and the
first use of any static member of the class triggers the class's
initializer, which runs each initializer once. *First use* means a
read or write of any static member, a call of a static method, or
the construction of an instance — including construction through
`super(...)` from a subclass. When several contexts reach the
first use at the same time, one runs the initializer and the
others wait; the value each of them then observes is the stored
one.

```nlang
class Config {
    public static int size = compute();   // runs on first use of Config
    public static int compute() { return 40; }
}
int main() {
    return Config.size;                   // initializer runs here, once
}
```

The initializer's stores are published by the initializer itself;
if it throws, the exception — the thrown object, or an
`InitializationException` (§8.3) wrapping it when the failure
escapes through the initialization protocol — propagates to the
first-use site, and the class's initialization state resets: the
next use runs the initializer again. *Retry repeats side effects:*
an initializer that partially succeeded before throwing runs from
its start on the retry, so observable effects can happen twice. A
plain counter field without an initializer survives the retry at
its stored value, which is the usual way to detect a repeated
attempt.

A read of an initializer-bearing static field from inside the very
initialization it awaits — directly, or around a cycle of two
classes whose initializers reference each other — cannot see the
value before the initializer stores it. That read raises
`InitializationException` (§8.3) at the reading site, and it is
catchable like every exception; the cycle `A`'s initializer reads
`B.b` while `B`'s reads `A.a` surfaces this way at the deeper
read.

## 4.8.9 Virtual methods and overriding

The `virtual` flag marks a method for dynamic dispatch. A call
through a class reference selects the implementation by walking the
class hierarchy from the object's runtime class upwards: the
most-derived implementation of the name wins. A call to a
non-virtual method binds to the statically declared type and never
dispatches dynamically: a subclass method that shares a
non-virtual ancestor method's name shadows it for calls through the
subclass type, while a call through the ancestor type still runs
the ancestor implementation.

```nlang
class Animal {
    public virtual int Sound() {
        return 0;
    }
}
class Dog : Animal {
    public int Sound() {
        return 1;
    }
}
```

Overriding requires no keyword: a same-name, same-signature method
in a subclass replaces the inherited implementation. A subclass may
call the parent implementation with `super(...)` only for
constructors; there is no `super.method(...)` form.

A subclass method that shares an ancestor virtual method's name
must match its signature exactly: the same parameter types and
count, the same return type, and the same `out` modifiers. A
mismatch is a compile-time error. Once a name is bound to a virtual
contract — an inherited virtual method or an implemented interface
method (§4.9.1) — the name must not be overloaded in the class or
any subclass: the same-name methods must map one-to-one onto the
contract. Within one class, a name with a `virtual` member cannot
be overloaded at all; make the whole family non-virtual to overload
it. The `Object` protocol methods are virtual and follow the same
rule (§4.11.2).

*Informative:* virtual dispatch selects methods by name — the call
site lays out arguments against the statically bound type's
signature, and at run time the nearest derived same-name method is
taken — which is why a signature mismatch would misinterpret the
argument and return-value bits. Fix the signature to override, or
rename the method to keep a same-name overload.

## 4.8.10 Identity, equality and null

Two class references compare by identity: `==` holds exactly when
both denote the same instance or both are `null`. The method
`equals` is a separate, opt-in operation (§4.11.2); overriding it
does not change `==`, and value equality requires an explicit
`equals` call.

It is a run-time error if a member is accessed or a method is
called through a `null` reference; the error is raised as a
`NullPointerException` (chapter 8).

*Informative:* `null` is heap index 0. An uncaught
`NullPointerException` terminates the program with exit code 1
(chapter 9). A `string` handle reads `null` as the empty string
instead (§4.3).

## 4.8.11 Classes across modules

An imported module's classes are full types: after `import lib;`
the `public` instance fields of `lib.B` are readable and writable,
its `public` instance methods callable (inherited members, default
arguments and overloads included), and virtual calls dispatch
dynamically exactly as within one unit.

```nlang
import lib;
class D : lib.B {
    public int f(int x) { return x + 10; }   // overrides lib.B.f
}
int main() {
    D d = D();
    return d.f(3);
}
```

```nlang lib.n
class B {
    public virtual int f(int x) { return x; }
}
```

`class D : lib.B` extends an imported base class and
`class C implements lib.I` implements an imported interface; the
override and `implements` contracts are enforced across the module
boundary exactly as within a unit. Visibility is the same rule
everywhere: only `public` members of an imported class are
reachable, `protected` members are reachable inside a subclass
body, and a non-`public` constructor cannot be called across the
boundary (§4.8.2).

## 4.8.12 Const Classes and Const Structs

`const class` and `const struct` declare type-level immutable
aggregates: the bindings of all instance fields are read-only
after construction.

```nlang
const class Note {
    public string text;
    public Note(string t) { text = t; }   // writable inside the ctor
}
const struct Vec {
    int x;
    int y;
}
```

- The writable window is the field-declaring type's own
  constructor body plus its field initializers: field defaults
  are injected at construction time as usual (§4.8.3). A write
  anywhere else — a method body, another class, another module —
  is a compile-time error naming the field and its owner. A
  subclass constructor must not rewrite inherited fields;
  supplements ride `super(...)` (§4.8.6).
- The marker does not freeze variable bindings: `Note n = a;
  n = b;` is legal — type-level const and a `const` local
  (binding-level, §6.1) are different things. `static` members
  are unaffected.
- Inheritance is sealed in both directions: a const class can
  only be extended by another const class — in `class D : B`, a
  const `B` with a non-const `D` is a compile-time error, and a
  const subclass of a plain base is rejected the same way (the
  root `Object` is exempt: it declares no fields, §4.11).
  Structs have no inheritance and are each sealed on their own
  (§4.7). The const marker rides the type record of the compiled
  module (§20.4), so writes to an imported const aggregate are
  rejected on the consumer side too (§4.8.11).
- A member read of a `List` or `Dict` field of a const aggregate
  is rewritten to a shallow copy — the built-in `copy()` (§12.1,
  §12.2): callers get a snapshot, and mutating the copy never
  touches the aggregate. Two reads yield distinct copies, so
  comparing them with `==` is meaningless. `string` fields (like
  `Exception.message`, §8.3) are immutable by themselves and are
  simply read-only.
- The field type surface is constrained, checked at declaration
  so the seal cannot be pierced: a field cannot be an array, a
  non-const class type or an interface type — a mutable handle
  would pierce the seal, and an interface reference may be backed
  by a mutable implementor — and the element types of `List` and
  `Dict` fields must be immutable-faced as well. The legal
  surface is primitives, `string`, enumerations, const classes
  and const structs, element-compliant containers, and `func`
  types. The built-in `Exception` family is entirely const
  (§8.3), which is what makes the self-referential `Exception
  cause` field legal.

*Informative:* the sealed-write diagnostic names the field and
its owner, for instance `The field "text" is immutable: it
belongs to the const class "Note" and only that class's
constructors may write it.`
