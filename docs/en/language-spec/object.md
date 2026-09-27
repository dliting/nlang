# Object & Boxing


`Object` is the implicit root of the class hierarchy. Every class that does not
explicitly inherit from another class implicitly inherits from `Object`.
Object is synthesized by the compiler — there is no source-level
`class Object { ... }` declaration, and users do not write `class Foo : Object`
(that syntax is rejected).

`Object` is a **reference type**: a value of type `Object` is a reference that
may point at a class object *or* a boxed primitive. Assignment and passing
copy the reference. See [Type Semantics](type-semantics.md).

### `equals` and `getHashCode`

Object provides two virtual methods with default identity semantics:

```nlang
int equals(Object other);    // identity: same heap reference → 1, else 0
int getHashCode();           // identity: heap index of `this` (or 0 for null)
```

`equals` and `getHashCode` are virtual via name-based dispatch — subclasses
override them simply by declaring a method with the same name (no `override`
keyword needed; the runtime walks the class hierarchy and finds the
most-derived implementation first):

```nlang
class Point {
    public int x;
    public int y;
    int getHashCode() {              // overrides Object.getHashCode
        return this.x * 31 + this.y;
    }
}
```

**String value semantics**: although string is a primitive, calls to
`string.getHashCode()` and `string.equals(string)` are intrinsified to use
*value* semantics (`std::hash` for hash, content comparison for `equals`).
This makes strings usable as `Dict` keys without a wrapper class. See
[String](string.md).

**`==` is unaffected by `equals`**: `Object.equals` is an opt-in method. The
`==` operator on class references compares heap indices directly. The reason
`equals` exists as a separate method is to allow user classes to override with
value equality without breaking identity-equality tests in the wider codebase.

### Boxing (primitive → Object)

A primitive value (int / float / string) is implicitly boxed when assigned to
an `Object`-typed target:

```nlang
Object o = 5;            // int boxed
Object f = 3.14;         // float boxed
Object s = "hi";         // string boxed

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 boxed at the call site
```

The runtime representation is a tagged boxed slot (slot[0] = type tag,
slot[1] = value bits). Boxed slots hold no references and are explicitly
skipped by the GC mark phase.

**`null` literal boxing preservation**: the literal `0` (used for `null`)
short-circuits `OP_Box` — no heap slot is allocated, and the value `0` remains
as the Object slot's contents. This keeps `Object o = null` and `Object o = 0`
as no-ops rather than wrapping 0 in a boxed-int heap ref.

### Unboxing and downcasting

Use the `as` operator to unbox a primitive back out of `Object`, or downcast
an `Object` to a class type. See [Type Casts](type-casts.md) "Runtime-checked
Cast (`as`)" for the full `as` rules (unbox, class downcast, `Object` upcast
special-casing, and which conversions are compile errors).
