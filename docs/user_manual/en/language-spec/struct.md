# Struct


A `struct` is a value-typed aggregate of fields. It holds data only — no
method bodies, no `this`. Use a `class` for behavior.

```nlang
struct Point {
    int x;
    int y;
}
```

### Value semantics

A struct is a **value type** with **deep-copy** semantics throughout the
language:

- **Declaration** (`Point p;`) zero-initializes every field.
- **Assignment / copy** (`Point q = p;`) copies the whole struct by value,
  including any nested struct fields, so the source and the copy are
  independent.
- **Parameter passing / return**: deep-copied into the callee's local frame
  and back out to the caller's result slot. The callee operates on its own
  copy.
- **As a class field**: the class owns an independent deep copy. Assigning
  `obj.s = s1` deep-copies `s1` into the class's field slot.
- **As an array element**: `new Point[n]` eagerly materializes a fresh,
  independent struct instance per element (including nested struct fields,
  recursively). Reading an element into a struct variable (`Point p = arr[i]`)
  deep-copies it; writing through a subscript (`arr[i].x = v`, `arr[i] = p`)
  stores into the array's own element. Zero-length struct arrays
  (`new Point[0]`) are legal — `.length` is 0 and no elements are materialized.

**Shallow copy of class references within structs**: when a struct contains a
class-typed field, the class reference (heap index) is copied as-is during
struct copy. Both the original and the copy refer to the same class object on
the heap. This is consistent with C#'s behavior for struct fields of reference
type.

```nlang
class Inner { public int x; }
struct Wrapper { public Inner ref; }

int main() {
    Inner obj = new Inner();
    obj.x = 10;
    Wrapper a;
    a.ref = obj;
    Wrapper b = a;       // shallow copy: b.ref == a.ref (same object)
    b.ref.x = 99;        // modifies the shared Inner object
    return a.ref.x;      // returns 99, not 10
}
```

### Fields

A field is read or written through the member expression `p.x`; a struct has no
method body, so there is no `this` to bind one to. A named initializer
(`new Point{x: 1, y: 2}`) sets the named fields; any field left out keeps its
zero value. See [Collection Initializers](collection-initializers.md) for the
full init-list rules.

### What a struct can contain

- Primitive fields (int, float, string, etc.)
- Enum fields (stored as int32)
- Struct fields (deep-copied, owned by the containing struct)
- Class fields (reference, shallow-copied)

Structs cannot contain methods. Use classes for behavior.

### Output

A struct has no `toString` and is **permanently excluded** from string
coercion — `"x" + structInstance` is a compile error (struct is a pure-data
type in NLang). See [Type Casts](type-casts.md) "Object.toString() Protocol".
