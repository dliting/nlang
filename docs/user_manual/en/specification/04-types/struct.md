# 4.7 Structs

## 4.7.1 Declaration and members

A struct is a value type declared with `struct` (§2.4). Its
members are fields and constructors; a struct has no methods.
Struct members are always public: fields and constructors default
to `public`, and spelling `private` or `protected` on a struct
member is a compile-time error.

```nlang
struct Point {
    int x;
    int y;
}
```

*Informative:* a struct is a plain data record with no hidden
state, so encapsulation rules do not apply to it; behavior lives
in classes.

## 4.7.2 Field initializers

A field may declare an initializer. The default value of a struct
is each field's default: the declared initializer when present,
otherwise that type's default.

A struct field may carry a compile-time constant initializer,
under the same expression rule as a class field (§4.8.3). Unlike
a class, the default is applied at every allocation rather than
injected into a constructor: a bare declaration, an array element,
a nested struct field and a partial initializer list all
materialize the defaults, and an initializer entry stores over
them; a copy copies the stored values and does not re-apply them.

```nlang
struct Point {
    int x = 3;
    int y = 4;
    string tag = "pt";
}
```

*Informative:* the defaults are stored in the compiled module's
struct record, so they stay visible after an `import`
([Module Serialization](../../vm-architecture/module-serialization.md)).

## 4.7.3 Constructors

A struct may declare constructors. A struct constructor is
declared with the syntax of a class constructor (§4.8.6) and may
overload (§7.3). A constructor runs only when the construction is
explicit — a constructor call `T(args)` or a direct-init
declaration `T x(args)`; a bare declaration and an initializer
list fill fields without running any constructor (§4.7.5).
Struct constructor calls take positional arguments only (§7.2).

```nlang
Point p(2, 3);                  // direct-init declaration
Point q = Point(1, 4);          // constructor call, assigned
Point r;                        // bare declaration: no constructor runs
Point s = Point{5};             // init list: no constructor runs
```

`new Point(...)` runs the constructor too; the value is placed on
the heap, matching the class-typed `new` (§4.8.4).

## 4.7.4 Value semantics

Assignment between struct values copies the whole value: every
field is copied, including nested struct fields, so the source and
the copy are independent. A field of a reference type copies its
reference, so the two struct values then share that one object
(chapter 10).

The same deep copy applies everywhere a struct value moves:

- **Declaration** (`Point p;`) gives every field its default value
  (§4.7.2).
- **Parameter passing and return** deep-copy the value once on the
  way in and once on the way back; the callee operates on its own
  copy.
- **As a class field**: assigning `obj.s = s1` deep-copies `s1`
  into the class's field.
- **As an array element**: `Point[n]` allocates a fresh,
  independent struct per element at creation, nested struct fields
  included. Reading an element into a struct variable deep-copies
  it; writing through a subscript stores into the array's own
  element (§4.10). A zero-length struct array is legal and
  creates no elements.

When a struct contains a class-typed field, the class reference is
copied as-is during the struct copy: the original and the copy
refer to the same class object on the heap.

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

*Informative:* the deep-copy-with-shared-references model matches
how mainstream languages with value aggregates treat
reference-typed members.

## 4.7.5 Creation and initialization

A brace initializer list fills fields by name or by position;
fields it omits keep their defaults.

- **Named entries** `Point{x: 1, y: 2}` assign by field name.
- **Positional entries** `Point{3, 7}` assign in field declaration
  order; a prefix of the fields may be given — `Point{5}` fills
  `x` only and leaves the rest at their defaults.

A field is read or written through the member expression `p.x`; a
constructor may use `this` to name a field (§4.8.7). The full
initializer-list rules, including the shapes a list may target,
are in chapter 5 and [Collection
Initializers](../05-expressions/initializers.md).

## 4.7.6 Structs across modules

An imported struct is a full module member: its `public` fields
are readable and writable across the module boundary, and a brace
initializer fills fields by name against the imported field list
([Packages](../03-program-structure/packages.md)).

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

Field access follows the same visibility rule as everywhere: only
`public` fields are reachable across the boundary — struct fields
are `public` unless spelled otherwise. If the struct declares a
constructor, `lib.Vec(3, 4)` calls it across the boundary. The
producer's field defaults ride the compiled module record, so an
imported struct declared without an initializer materializes them
just as within the defining module (§4.7.2).

## 4.7.7 Output

A struct has no `toString` and takes no part in string coercion:
`"x" + structValue` is a compile-time error. See
[Type Casts](casts.md) for the `toString` protocol.
