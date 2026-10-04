# Const Local


```nlang
const int X = 5;
const string Greeting = "hello";
```

Local variables marked `const` must be initialized at declaration and cannot
subsequently be assigned or compound-assigned. Only **local** const is
supported; class/struct field const is not. See
[Variable Declaration](declarations.md) for the general form.

**Const is shallow (Java-`final`-style)**: `const` prevents rebinding the *name*
but does not freeze the referenced object's state. Member mutation through a
const local is allowed:

```nlang
const Foo f = new Foo();
f.x = 5;            // OK — f itself is not reassigned
f = new Foo();      // ERROR — cannot reassign const local
const Point p = q;
p.x = 5;            // OK — only p's binding is const
```

For class fields and array elements this means: a `const` reference still
permits writing through it. Deep/immutability-style const is not supported and
may be revisited later.
