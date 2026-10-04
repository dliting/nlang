# Class


A `class` is a reference-typed object: it lives on the heap and is accessed
through a reference (heap index). It carries fields, constructors, methods,
and a single inheritance line.

```nlang
class Node {
    public int value;
    public Node next;

    Node(int v) {
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

Fields carry access modifiers `public` / `private` / `protected`. NLang's
default access modifier is `private`.

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

**Override**: a subclass method that has the **same name and signature** as a
parent virtual method overrides it — there is no `override` keyword (unlike C++'s
`override` or Java's `@Override`); name-and-signature matching is decided by
the compiler.

`Object`'s two virtual methods (`equals`/`getHashCode`) are overridden the same
way — a subclass declares the method with the same name (see
[Object & Boxing](object.md)). Interface virtual dispatch uses the same
mechanism (see [Interface](interface.md)). Quick tour:
[Classes and Inheritance](../getting-started/classes-and-inheritance.md).

### super() — constructor chaining

```nlang
class Base {
    public int v;
    public int Base(int x) { this.v = x; return 0; }
}
class Kid : Base {
    public int Kid(int x) {
        super(x * 2);        // calls Base(int)
        return 0;
    }
}
```

- `super(args);` invokes the **direct parent class's constructor** on the same
  `this` object. Valid only inside a constructor of a class that has a parent
  (`Object` has none).
- May appear at **any statement position** in the ctor (not restricted to the
  first statement).
- Argument count must match the parent ctor's parameter count. For parents in
  the built-in Exception family the ctor takes exactly one `message` argument.
- Named arguments (`super(x = 1)`) are not supported (compile error).
- `super()` with no arguments against a parent with no constructor is a legal
  no-op; passing arguments in that case is a compile error.

### Null check

A class-typed variable can be null (represented as heap index 0). Accessing a
field or method on a null class reference throws a `NullPointerException`,
which can be caught by a `try/catch` block. If uncaught, the program
terminates with exit code 1. See [Exceptions](exception.md). A string handle
reads null as the empty string instead — see [String](string.md).
