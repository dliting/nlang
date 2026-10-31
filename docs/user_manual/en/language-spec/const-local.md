# Const Local

The normative rules are in [specification §6.1](../specification/06-statements.md).
```nlang
const int X = 5;
const string Greeting = "hello";
```

Local variables marked `const` must be initialized at declaration and cannot
subsequently be assigned or compound-assigned. Function parameters may
carry `const` too (the callee may not reassign them); `const` on a
class/struct field is a syntax error. See
[Variable Declaration](declarations.md) for the general form.

**Const is shallow (Java-`final`-style)**: `const` prevents rebinding the *name*
but does not freeze the referenced object's state. Member mutation through a
const local is allowed:

```nlang
const Foo f = Foo();
f.x = 5;            // OK — f itself is not reassigned
f = Foo();      // ERROR — cannot reassign const local
const Point p = q;
p.x = 5;            // OK — only p's binding is const
```

For class fields and array elements this means: a `const` reference still
permits writing through it. Deep/immutability-style const is not supported.

To make a whole aggregate immutable, use **type-level** const — the `const
class` / `const struct` declarations (see [Class](class.md) "const classes
and const structs" and [Struct](struct.md) "Const struct"): instance fields
become read-only after construction, and container field reads return
copies. The two are orthogonal: a variable of a const type can still be
rebound, while a const local on this page cannot. A `const` on a field
declaration (say `const int x;` as a field) remains a syntax error —
immutability is granted wholesale by the type-level marker, never "half a
class".
