# Class

The normative rules are in [specification §4.7](../specification/04-types.md).

A `class` is a reference-typed object: it lives on the heap and is accessed
through a reference (heap index). It carries fields, constructors, methods,
and a single inheritance line.

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

### Reference semantics

A class follows **reference semantics** throughout the language:

- **Assignment** (`obj2 = obj1`) copies the reference (heap index). Both
  variables point at the same object.
- **Parameter passing**: class arguments pass the reference. The callee can
  modify the object's fields, and the caller sees the changes.
- **Return value**: returns the reference. No copy is made.
- **As a struct field**: the struct stores the reference (heap index); a
  struct copy shallow-copies this reference. See [Struct](struct.md).

The full value-vs-reference summary is on [Type Semantics](type-semantics.md).

### Fields and access modifiers

Fields carry access modifiers `public` / `private` / `protected`. Class
members default to `private`; interface members are the opposite and
default to `public` (see [Interface](interface.md)).

- `public` — reachable wherever the class itself is visible.
- `private` — reachable only inside the declaring class's own methods.
- `protected` — reachable inside the declaring class and inside the
  bodies of its subclasses (by bare name or through the base type
  name); everywhere else it is inaccessible.
- Constructors obey the same rule: a non-`public` constructor cannot
  be called from outside the class
  (`constructor of class "C" is not accessible`).

The same visibility rules hold across module boundaries — see
[Classes across modules](#classes-across-modules).

### Creating instances

```nlang
Node a = Node(42);        // constructor call: heap-allocates and runs the ctor
Node b(42);               // direct-init declaration, equivalent to the line above
Node c;                   // declaration only: c is null
```

- A **constructor call** `Node(42)` is an expression: it allocates the
  object on the heap, runs the constructor with the arguments, and
  returns the reference. Writing `P()` for a class without a constructor
  is legal too — it just allocates and zero-initializes the fields.
  Inside the class body the class's own name still denotes a
  constructor call — `Node(7)` in a method of `Node` creates a new
  object.
- The **anchored brace form** `P{value: 42}` fills fields with
  `field:value` entries: the class must have a parameterless
  constructor (explicit or implicit) — this page's `Node`, whose
  constructor takes `Node(int v)`, does not qualify; fields are
  assigned by name after construction. See [Collection
  Initializers](collection-initializers.md).
- The declaration-site forms work on **every** declarator of a
  comma-separated list: `Node a(1), b(42);` and `Node arr[3];` are both
  legal — see the declarator forms in [Declarations](declarations.md).
- A declaration without an initializer (`Node c;`) yields null — see the
  default value table in [Declarations](declarations.md) and Null check
  below.

### Inheritance layout

Object memory layout is
`[typeId, ancestor_fields..., parent_fields..., own_fields...]`.
The type ID in the first slot identifies the runtime class for virtual
dispatch.

### Constructors

Constructors are optional and named after the class. Only the direct class's
constructor is called; ancestor constructors are not automatically invoked.
A subclass ctor can forward to the direct parent's constructor with
`super(args);` (see "super() — constructor chaining" below). Without an
explicit `super()`, fields inherited from ancestors are zero-initialized.

**Implicit `this.field` (bare member access)**: inside a method or constructor,
a bare identifier that resolves to a field of the enclosing class (including
inherited fields) is an implicit `this.field` access. Works for reads,
assignments, compound assignments (`v += 1`), and default-parameter
expressions (`int add(int x, int y = v)`). A local variable or parameter with
the same name shadows the field, matching Java/C# semantics.

### Field initializers

A class field may carry a compile-time constant default:

```nlang
class Cfg {
    public int x = 5;
    public string s = "hi";
    public float f = -1.5;
}
```

- The initializer must be a **constant expression**: null, bool, char,
  int-family, float-family, and string literals, plus unary negation — it
  may not reference another field or `this` (neither exists yet at
  initialization).
- The default is **injected into every constructor** before its body runs,
  base-first (matching C#/Java): a constructor that calls `super(...)` gets
  the injection right after the `super(...)`; one with no `super(...)` gets
  it at the top of the body. When the class declares no constructor at all, a
  **synthesized public zero-arg constructor** carries the defaults, so `Cfg()`
  and the brace form `Cfg{}` both apply them.
- With a field default present, `super(...)` must be the constructor's
  **first statement** — otherwise the injection point is undefined and the
  constructor is a compile error.
- A `native` or bodyless constructor cannot carry field defaults (compile
  error).

### Static methods

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

A `static` method belongs to the class, not to any instance:

- **Call it through the type name**: `Util.twice(21)` from outside the
  class (the method must be `public` to be reachable). Calling a static
  through an object is rejected (`must be called through the type
  name`), and calling an instance method through the type name is
  rejected too (`must be called through a receiver object`) — the two
  call shapes are kept apart.
- **Bare calls inside the class**: a static method calls sibling statics
  by bare name (`quad` calls `twice`), and a non-static method calls a
  static the same way.
- **No `this`**: a static method has no receiver — `this` and the
  implicit `this.field` binding of a bare field name are compile errors
  (`this is not available in a static method`).
- **Inheritance**: a subclass inherits the parent's statics;
  `Sub.bid()` resolves up the chain.
- **Separate namespace**: a static and an instance member may share a
  name with different parameter lists; an exact same-signature collision
  is a compile error. Statics never participate in virtual dispatch
  (below).

### Virtual methods and overrides

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

The `virtual` keyword marks a method as **virtual**: when the method is called
through a reference, the runtime walks the class hierarchy up from the object's
actual type, and the **most-derived implementation wins** (dynamic dispatch).
The type ID in the object's first layout slot is what virtual dispatch uses.

A subclass method that shares an ancestor virtual method's name but differs in
a parameter or return type is a compile error: virtual dispatch selects
methods by name — the call site lays out arguments against the statically
bound type's signature, and at run time the nearest derived same-name method
is taken, so a signature mismatch reads arguments and the return value as
wrong bits. Once a name is bound to a virtual-method contract (a same-name
ancestor virtual method, or a same-name method of an interface the class
`implements`), it **must not be overloaded** in the subclass: the subclass's
set of same-name methods must map one-to-one onto the contract signature
(exactly the same parameter types and count and return type — the `out`
modifier must also match). The same holds **within one class**: a name
that has a `virtual` member cannot be overloaded there at all — virtual
dispatch selects by name, so the family must share one signature; make
the whole family non-virtual to overload it. Fix the signature to
override; rename the method
to keep a same-name method with a different signature. The `Object` protocol
methods (`equals`/`getHashCode`/`toString`) follow the same rule; the
canonical signatures are on [Object & Boxing](object.md).

`Object`'s three virtual methods (`equals`/`getHashCode`/`toString`) are
overridden the same way — a subclass declares the method with the same name (see
[Object & Boxing](object.md)); `==` comparison is unaffected by an `equals`
override — class `==` is always identity, and value equality requires an
explicit `equals` call. Interface virtual dispatch uses the same
mechanism (see [Interface](interface.md)). Quick tour:
[Classes and Inheritance](../getting-started/classes-and-inheritance.md).

### super() — constructor chaining

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

- `super(args);` invokes the **direct parent class's constructor** on the same
  `this` object. Valid only inside a constructor of a class that has a parent
  (`Object` has none).
- May appear at **any statement position** in the ctor (not restricted to the
  first statement) — except when the class carries a field default, which
  requires `super(...)` to be the first statement (see [Field
  initializers](#field-initializers) below).
- Argument count must match the parent ctor's parameter count. For parents in
  the built-in Exception family the ctor takes exactly one `message` argument.
- Named arguments (`super(x = 1)`) are not supported (compile error).
- `super()` with no arguments against a parent with no constructor is a legal
  no-op; passing arguments in that case is a compile error.

### Classes across modules

An imported module's classes are full types: after `import lib;` the
`public` instance fields of `lib.B` are readable and writable, its
`public` instance methods callable (inherited members, default
arguments, and overloads included), and virtual calls dispatch
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

- **Inheritance and contracts**: `class D : lib.B` extends an imported
  base class, `class C implements lib.I` implements an imported
  interface. The override and `implements` contracts are enforced
  across the module boundary exactly as within a unit: a same-name
  method whose signature differs from the inherited virtual method, a
  missing interface member, or a static method colliding with an
  inherited virtual method is a compile error.
- **Visibility is the same rule everywhere**: only `public` members of
  an imported class are reachable from the importing module; `private`
  members are not, and `protected` members are reachable inside a
  subclass body. A non-`public` constructor cannot be called across
  the boundary.
- **External structs and enums**: an imported `struct`'s `public`
  fields are readable and writable across the module boundary (and
  the anchored brace form `lib.Vec v = {x: 1, y: 2};` fills fields by
  name), and an `enum` is a full module member — its type, values,
  member methods, and `toString` all work through the package name
  (`lib.Color.Red`, `lib.Color.rank()`).

### Null check

A class-typed variable can be null (represented as heap index 0). Accessing a
field or method on a null class reference throws a `NullPointerException`,
which can be caught by a `try/catch` block. If uncaught, the program
terminates with exit code 1. See [Exceptions](exception.md). A string handle
reads null as the empty string instead — see [String](string.md).
