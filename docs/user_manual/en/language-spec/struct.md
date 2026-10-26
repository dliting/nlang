# Struct

The normative rules are in [specification §4.6](../specification/04-types.md).

A `struct` is a value-typed aggregate of fields. It holds data plus
optional constructors — no general method bodies. Use a `class` for
behavior.

```nlang
struct Point {
    int x;
    int y;
}
```

Struct members are always publicly accessible: fields and constructors
default to `public`, and the only modifier you may spell out is `public`
itself — writing `private` or `protected` on a struct member is a compile
error (`The struct member "%s" must be public.`). A struct is a plain data
record with no hidden state, so visibility rules do not apply to it.

### Value semantics

A struct is a **value type** with **deep-copy** semantics throughout the
language:

- **Declaration** (`Point p;`) gives every field its default value —
  zero when no initializer is declared, the declared default when one is
  (see [Field initializers](#field-initializers) below).
- **Assignment / copy** (`Point q = p;`) copies the whole struct by value,
  including any nested struct fields, so the source and the copy are
  independent.
- **Parameter passing / return**: deep-copied into the callee's local frame
  and back out to the caller's result slot. The callee operates on its own
  copy.
- **As a class field**: the class owns an independent deep copy. Assigning
  `obj.s = s1` deep-copies `s1` into the class's field slot.
- **As an array element**: `Point[n]` eagerly materializes a fresh,
  independent struct instance per element (including nested struct fields,
  recursively). Reading an element into a struct variable (`Point p = arr[i]`)
  deep-copies it; writing through a subscript (`arr[i].x = v`, `arr[i] = p`)
  stores into the array's own element. Zero-length struct arrays
  (`Point[0]`) are legal — `.length()` is 0 and no elements are materialized.

**Shallow copy of class references within structs**: when a struct contains a
class-typed field, the class reference (heap index) is copied as-is during
struct copy. Both the original and the copy refer to the same class object on
the heap. This is consistent with C#'s behavior for struct fields of reference
type.

```nlang
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // shallow copy: b.ref == a.ref (same object)
    b.ref.x = 99;        // modifies the shared Inner object
    return a.ref.x;      // returns 99, not 10
}
```

### Creating and initializing

A struct may declare constructors with the same syntax as a class (optional
`public` modifier, default parameters, overloads, and implicit `this`
bare-field access — see [Class](class.md) "Constructors"). A constructor runs
**only on explicit construction**:

```nlang
Point p(2, 3);                  // direct-init declaration
Point q = Point(1, 4);          // constructor call, assigned
Point r;                        // bare declaration: no constructor runs
Point s = Point{5};             // init list: no constructor runs
```

- **Constructor call** `Point(1, 4)` and **direct-init** `Point p(2, 3);`
  run the constructor. `new Point(...)` does too (the value is placed on the
  heap, matching the class-typed `new` — see [Class](class.md)).
- **Bare declaration** `Point r;` and **init lists** `Point{...}` do **not**
  run a constructor — every field takes its default value (zero when no
  initializer, the declared default when one is). Array elements, nested
  struct fields, and a deep copy behave the same way.
- **Named entries** `Point{x: 1, y: 2}`: assign by field name; fields
  left out keep their default value.
- **Positional entries** `Point{3, 7}`: assign in field declaration
  order; you may give only the first few — `Point{5}` fills `x` only and
  leaves the rest at their defaults.
- A struct array `Point[3]` eagerly materializes a fresh instance per
  element — see reference semantics above and [Array](array.md).

A field is read or written through the member expression `p.x`; a constructor
may use `this` to name a field. See [Collection
Initializers](collection-initializers.md) for the full init-list rules (target
shapes and disambiguation).

### Field initializers

A struct field may carry a compile-time constant default, like a class field
(see [Class](class.md) "Field initializers"):

```nlang
struct Point {
    int x = 3;
    int y = 4;
    string tag = "pt";
}
```

Unlike a class, a struct default is **not injected into a constructor** — it
is applied at every allocation. A bare declaration, an array element, a
nested field, and a partial init list all materialize the defaults (an
init-list entry stores over them). A deep copy copies the stored values and
does not re-apply them. The defaults ride the `.ncu` struct record, so they
stay visible after an `import` — see [Module
Serialization](../vm-architecture/module-serialization.md).

### What a struct can contain

- Primitive fields (int, float, string, etc.)
- Enum fields (stored as int32)
- Struct fields (deep-copied, owned by the containing struct)
- Class fields (reference, shallow-copied)
- Constructors (same syntax as a class)

Structs cannot contain general methods. Use classes for behavior.

### Structs across modules

An imported `struct` is a full module member: its `public` fields are
readable and writable across the module boundary, and the anchored brace
form fills fields by name (see [Classes across
modules](class.md#classes-across-modules) for the shared import model).

```nlang lib.n
struct Vec {
    int x;
    int y;
}
```

```nlang
import lib;

lib.Vec v = {x: 1, y: 2};   // brace init by field name
v.x = 10;                   // write an imported field
int sum = v.x + v.y;        // read an imported field
```

- **Field access follows the same visibility rule as everywhere**: only
  `public` fields are reachable across the boundary; a `private` field is
  not (struct fields are `public` unless spelled otherwise).
- **Named init and construction**: `lib.Vec v = {x: 1, y: 2};` fills fields
  by name against the stub's rebuilt field list; if the struct declares a
  constructor, `lib.Vec(3, 4)` calls it across the boundary.
- **Field defaults still apply at allocation**: the producer's field
  defaults ride the `.ncu` record, so an imported struct declared without
  an initializer materializes them just as within the defining module.

### Output

A struct has no `toString` and is **permanently excluded** from string
coercion — `"x" + structInstance` is a compile error (struct is a pure-data
type in NLang). See [Type Casts](type-casts.md) "Object.toString() Protocol".
