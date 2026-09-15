# Type Semantics


### Struct: Value Semantics

Structs follow value semantics throughout the language:

- **Assignment**: `s2 = s1` creates a deep copy. `s2` is an independent
  instance — modifying `s2` does not affect `s1`.
- **Parameter passing**: Struct arguments are deep-copied into the callee's
  local frame. The callee operates on its own copy.
- **Return value**: A struct return value is deep-copied to the caller's
  result slot.
- **Class field**: When a struct is a class field, the class owns an
  independent deep copy. Assigning `obj.s = s1` deep-copies `s1` into the
  class's field slot.
- **Array element** (Phase 9d-3): `new Point[n]` eagerly materializes a
  fresh, independent struct instance per element (including nested struct
  fields, recursively). Reading an element into a struct variable
  (`Point p = arr[i]`) deep-copies it; writing through a subscript
  (`arr[i].x = v`, `arr[i] = p`) stores into the array's own element.
  Zero-length struct arrays (`new Point[0]`) are legal — `.length` is 0
  and no elements are materialized.

**Shallow copy of class references within structs**: When a struct contains a
class-typed field, the class reference (heap index) is copied as-is during
struct copy. Both the original and the copy refer to the same class object on
the heap. This is consistent with C#'s behavior for struct fields of reference
type.

Example:
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

### Class: Reference Semantics

Classes follow reference semantics:

- **Assignment**: `obj2 = obj1` copies the reference (heap index). Both
  variables point to the same object.
- **Parameter passing**: Class arguments pass the reference. The callee can
  modify the object's fields, and the caller sees the changes.
- **Return value**: Returns the reference. No copy is made.
- **Struct field**: When a class is a struct field, the struct stores the
  reference (heap index). Struct copy shallow-copies this reference.

### Summary Table

| Operation          | struct          | class           |
|--------------------|-----------------|-----------------|
| Assignment         | Deep copy       | Copy reference  |
| Parameter passing  | Deep copy       | Pass reference  |
| Return value       | Deep copy       | Return reference|
| As class field     | Deep copy owned | Store reference |
| As struct field    | Deep copy owned | Store reference |
