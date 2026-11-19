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

Assignment between struct values writes the source's value into the
record the target position already owns: every field cell is
overwritten, including nested struct fields, so the source and the
target stay independent. A field of a reference type copies its
reference, so the two struct values then share that one object
(chapter 9).

Every struct storage position — a local variable, a field, an array
element, a container element, a static slot — owns one heap record
for its entire life; a write into an existing position is an **in-
place update**: it overwrites that record's field cells and
allocates nothing. Only establishing a new storage position
allocates a record and fills it with the initial value:

- **Declaration** (`Point p;`) establishes the local's record and
  gives every field its default value (§4.7.2).
- **Parameter passing and return** each establish a fresh record
  copied from the source: once on the way in and once on the way
  back; the callee operates on its own copy.
- **As a class field**: assigning `obj.s = s1` writes `s1`'s value
  in place into the record the field already owns.
- **As a static field's value**: a class or interface static slot
  (a static field, a static constant's lazy channel, or a static
  local of struct type) owns one record for life — the first store
  establishes it (or fills the type-default record an
  initializer-less slot starts at), and every later store updates
  it in place; reading the slot into a variable copies the value
  into a new record, and a member write through the slot mutates
  the slot's own record (§4.8.8).
- **As an array element**: `Point[n]` establishes a fresh,
  independent record per element at creation, nested struct fields
  included. Reading an element into a struct variable copies the
  value; writing through a subscript updates that element's
  existing record in place (§4.10). A zero-length struct array is
  legal and creates no elements.
- **As a container element**: `List<T>` and `Dict<K,T>` `set`
  writes a struct value in place into the record that position
  owns; storing under a new key establishes a record for the new
  position (chapter 12).
- **Equality** (`==` / `!=`) compares by field value: nested struct
  fields recurse, `string` fields compare content, and reference-
  shaped fields (class, array, interface, `func`) compare identity —
  exactly what the record stores. Two structs of different types
  never compare equal, and ordering a struct is a compile error
  (§4.5).

When a struct contains a class-typed field, the class reference is
copied as-is during the copy: the original and the copy
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

## 4.7.8 Const Struct

A `const struct` brings type-level immutability to structs; the
full rules are §4.8.12. Fields are writable only inside the
struct's own constructor body and its field initializers; writes
anywhere else are compile-time errors. A `List` or `Dict` field
read returns a copy (§12.1, §12.2), and the field type surface is
constrained the same way. Structs have no inheritance, so each
declaration is sealed on its own.

```nlang
const struct Size {
    int w;
    int h;
}

int main() {
    Size s = Size{3, 4};   // init list fills at allocation time — legal
    int area = s.w * s.h;  // reads are normal
    // s.w = 5;            // compile error: "w" is immutable
    return area;
}
```

## 4.7.9 References (`T&`)

Value types — scalars, enums, strings and struct records — keep value
semantics by default: assignment copies, and each name owns its own
storage. The `&` mark opts a specific name into **aliasing existing
storage** instead of copying it:

```nlang
int q = 10;
int& r = q;    // r names q's storage — no copy is made
r = 20;        // writes q's storage: q == 20 now
```

**`r = q` never rebinds.** An assignment to a reference writes the
storage it already names. A reference is bound once, at its declaration,
and keeps pointing there for its whole lifetime — to aim elsewhere,
declare a new reference. The contrast:

```nlang
int a = 1;
int b = 2;
int& r = a;
r = b;         // writes b's VALUE into a's storage: a == 2, r still names a
int& s = r;    // compile error: "cannot bind a reference to reference"
```

References appear in four positions:

- **Local bindings** — `int& r = q;` (the examples above).
- **Formals** — `void bump(int& v)`: writes through `v` land in the
  caller's argument (§7.2).
- **Returns** — `Point& corner()`: the call result names storage owned
  elsewhere; returning a reference to a local fails with
  "cannot return a reference to local …; its storage ends with the
  call." Reference returns are implemented for free functions and
  class methods; an interface or enum method that declares one is a
  compile-time error.
- **Loop variables** — `foreach (int& n in nums)`: each pass names the
  current element storage (§6.8).

**The bindable family**: scalar, string and struct storage — locals,
formals, struct fields, array elements and container elements reached
through a value of that family. Identity types (class, array,
interface, `func`) are references already; `&` on them is an equivalent
notation. The binding requires the **exact** type — a conversion would
build new storage and break the aliasing, so
`cannot bind int to long&; a reference binding requires the exact
type.` also rejects enum-to-integer and widening shapes.

**Record stabilization** is the model that makes bindings safe: a
struct local's storage is one stable record. Assignment `b = a` writes
that record's cells in place; a fresh record is only established at
creation scenes — a declaration without a copy, array element
creation, initializer materialization. The record a binding names
never moves, so the binding never dangles.

**Read-only references** (`const T&`, §7.2) bind const storage and
constants without allowing writes through the binding; an expression
result materializes into a temporary that the binding keeps alive. A
writable `&` to const storage is rejected:
"cannot bind a reference to const storage …; a reference may write
through its target."

Also rejected at compile time: a reference to a slice ("a reference
cannot bind a slice; slices are views, not element storage"), a
reference to an enum member ("cannot bind a reference to enum member"),
a reference to a method call result, and a `&` loop variable over a
`Dict` or `string` source.
