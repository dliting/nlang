# Declarations


### Import Declaration

```nlang
import io;                 // built-in package
import lib;                // external lib.nmod
import utils.helper;       // project file utils/helper.n
import utils.*;            // recursive wildcard
```

An `import` declares which modules this **file** may reference — the
import set belongs to the translation unit and never leaks to other
files. Three sources share one syntax:

| Source | Module path | Example |
|---|---|---|
| Project file | dotted path relative to the `.nproj` root: directory path + file stem | `utils/helper.n` → `utils.helper`; root `main.n` → `main` |
| External `.nmod` | file stem (single segment) | `lib.nmod` → `lib` |
| Built-in package | `io` / `math` / `fs` (preset modules) | `io` |

Visibility:

| Reference | Import needed? | Call form |
|---|---|---|
| Same file | no | bare |
| Same directory, other project files | no (implicit) | bare **or** qualified |
| Cross-directory, same project | **yes** (`import utils.helper;` or `import utils.*;`) | qualified only: `utils.helper.f()` |
| External `.nmod` | **yes** (`import lib;`) | qualified only: `lib.f()` |
| Built-in `io`/`math`/`fs` | **yes** (`import io;`) | qualified: `io.print` |

- Bare-name resolution covers only the own file plus same-directory
  files; everything else must be qualified by module path. Ownerless
  symbols (root built-ins such as the `Exception` class family, native
  host bindings) stay globally bare-visible.
- Wildcard `import utils.*;` is a **recursive prefix match** in module
  path space: every path starting with `utils.` is importable
  (`utils.helper`, `utils.sub.x`, ...). It only abbreviates the import
  list — calls still write the full path. Wildcards match project files
  only; external `.nmod` names are single-segment and never match.
- `import utils;` matches only the root file `utils.n`; to reach the
  `utils/` directory use the full path or a wildcard.
- Duplicate imports are idempotent; exact + wildcard overlap takes the
  union; importing the own module path or a same-directory file is a
  harmless redundancy.
- Resolution order for an import target: built-in → project file →
  external `.nmod` (via `-I`). No implicit fallback.
- Two units resolving to the same dotted package in one build are a
  compile error naming both source paths. A project directory named
  `io`/`math`/`fs` is an ordinary directory; only one package of each
  name may exist. Dotted imports resolve library sources under the
  matched search root (`-I <root>` + `<root>/a/b/c.n` addresses
  `import a.b.c;`); precompiled dotted packages (`.nmod`) still search
  by their single last segment until a later phase.

Diagnostics (examples):

```
Module 'utils.helper' is not imported. Add 'import utils.helper;' (or 'import utils.*;') at the top of this file.
Package 'io' is not imported. Add 'import io;' at the top of this file.
Module 'utils.helper' not found. Check the project Sources list or -I import path.
String import is removed. Use 'import <module>;' with an identifier path.
Function 'add' is not visible here. It lives in module 'utils.helper'; import it and qualify the call.
```

### Variable Declaration

```nlang
int x = 5;
float y = 3.14;
string s = "hello";
Color c = Color.Red;       // enum
Point pt;                   // struct (zero-initialized)
Node n = new Node();        // class (heap-allocated)
Node n2;                    // class (null)
```

Struct variables are zero-initialized (all fields = 0). Class variables
default to null.

### Enum Declaration

```nlang
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

Enum values are int32 at runtime. Members can be explicit or auto-incremented.

#### Enum Methods

```nlang
enum Color {
    Red = 1, Green = 2, Blue = 4;

    public int weight(int base) {
        return this * base;
    }

    public int isPrimary() {
        switch (this) {
            case Color.Red, Color.Green, Color.Blue: return 1;
        }
        return 0;
    }
}
```

Methods are declared after the member list, separated by a `;`. A lone
trailing `;` with no methods (`enum E { A; }`) is accepted Java-style
closing syntax. Every method must have a body (abstract methods are
rejected).

- **`this` is the enum value.** Inside a method, `this` is the int32
  value of the receiver: it participates in arithmetic and `switch`
  directly (`this * base`, `switch (this)`), with no boxing. Methods
  are statically dispatched (`OP_CallMethodDirect`) — enums have no
  inheritance and no virtual dispatch.
- **Call through a receiver.** `c.weight(3)`, `this.weight(3)`,
  `Color.Red.weight(3)` — any enum-valued expression works. A bare
  `weight(3)` (receiver-less) is a compile error, matching class
  methods.
- **Parameters:** no default values (`int f(int a, int b = 5)` is
  rejected) and no `out` parameters.
- **`toString` is reserved.** The built-in `toString()` intrinsic
  (value → name) cannot be shadowed by a user method.
- **`this.<member>` resolves as a constant.** `this.Red` inside a
  method reads the member `Red`'s value — it does not compare against
  the receiver. Convenient, but easy to misread; name receivers
  explicitly when in doubt.
- **Member/method name sharing is a conflict** (`enum E { f; int f() {...} }`
  is rejected). Access modifiers follow class-method rules.
- **Cross-module enums are not supported.** An enum type declared in an
  imported module is not visible to the importer (the `.nmod` format
  serializes enum names only, not declarations) — this is a limitation
  of the module format, not specific to methods.
- Arrays of enums: methods cannot be called on the array itself — index
  an element first (`a[i].rank()`, not `a.rank()`).

### Struct Declaration

```nlang
struct Point {
    int x;
    int y;
}

int main() {
    Point p;                       // zero-initialized: x = 0, y = 0
    p.x = 3;
    p.y = 4;
    Point q = p;                   // deep copy: q.x, q.y independent of p
    q.x = 9;
    Point r = new Point{x: 1, y: 2};   // named initializer
    return p.x + p.y + q.x + q.y + r.x + r.y;   // 3+4+9+4+1+2 = 23
}
```

Structs are value types:
- Declaring a variable (`Point p;`) zero-initializes every field.
- Copying (`Point q = p;`) copies the whole struct by value, including any
  nested struct fields, so the source and the copy are independent.
- A field is read or written through the member expression `p.x`; a struct
  has no method body, so there is no `this` to bind one to.
- A named initializer (`new Point{x: 1, y: 2}`) sets the named fields; any
  field left out keeps its zero value.

Structs can contain:
- Primitive fields (int, float, string, etc.)
- Enum fields (stored as int32)
- Struct fields (deep-copied, owned by the containing struct)
- Class fields (reference, shallow-copied)

Structs cannot contain methods. Use classes for behavior.

### Class Declaration

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

Classes support:
- Fields with access modifiers (public/private/protected)
- Constructors (optional, named after the class)
- Methods (implicit `this` parameter)
- Single inheritance (`class Child : Parent`)
- Virtual methods (`virtual` keyword, dynamic dispatch)
- Method overriding in subclasses

**Inheritance layout**: Object memory layout is `[typeId, ancestor_fields..., parent_fields..., own_fields...]`.
The type ID in the first slot identifies the runtime class for virtual dispatch.

**Constructor behavior**: Only the direct class's constructor is called;
ancestor constructors are not automatically invoked. A subclass ctor can
forward to the direct parent's constructor with `super(args);` (see
"super() — constructor chaining" below). Without an explicit `super()`,
fields inherited from ancestors are zero-initialized.

**Implicit `this.field` (bare member access)**: inside a method or
constructor, a bare identifier that resolves to a field of the enclosing
class (including inherited fields) is an implicit `this.field` access.
Works for reads, assignments, compound assignments (`v += 1`), and
default-parameter expressions (`int add(int x, int y = v)`). A local
variable or parameter with the same name shadows the field, matching
Java/C# semantics.

### Virtual Methods and Overrides

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

The `virtual` keyword marks a method as **virtual**: when the method is
called through a reference, the runtime walks the class hierarchy up
from the object's actual type, and the **most-derived implementation
wins** (dynamic dispatch). The type ID in the object's first layout slot
is what virtual dispatch uses (see "Class Declaration" above).

**Override**: a subclass method that has the **same name and signature**
as a parent virtual method overrides it — there is no `override` keyword
(unlike C++'s `override` or Java's `@Override`); name-and-signature
matching is decided by the compiler.

`Object`'s two virtual methods (`equals`/`getHashCode`) are overridden
the same way — a subclass declares the method with the same name
(example in the "Implicit Object Base Class" section on this page).
Interface virtual dispatch uses the same mechanism (see the `TotalArea`
example in the "Interface Declaration" section on this page); quick
tour: [Classes and Inheritance](../getting-started/classes-and-inheritance.md).

### Interface Declaration

```nlang
interface IShape {
    public int Area();
    public int Perimeter();
}

class Square implements IShape {
    public int side;
    public int Area() { return this.side * this.side; }
    public int Perimeter() { return 4 * this.side; }
}

int TotalArea(IShape s) {
    return s.Area();   // virtual dispatch through the interface
}
```

Interfaces support:
- Method signatures only (no fields, no implementation)
- `class X implements IShape` — a class declares conformance with the
  `implements` keyword (the `:` form is for class inheritance, not
  interface conformance; `class X : IShape` is rejected)
- Virtual dispatch on interface-typed locals/params/fields
- Polymorphic collections (`List<IShape>` of mixed `Square`/`Circle`)

**Method `public` keyword is required** in interface declarations.
NLang's default access modifier is `private`; an interface method
declared as `int m();` (no `public`) is parsed but treated as private
and **not accessible** from call sites. The resulting error message
— "The function X does not exist or is not accessible" — is
misleading because the method does exist, just isn't public. Always
write `public int m();` in interfaces.

### Implicit `Object` Base Class

Every class that does not explicitly inherit from another class implicitly
inherits from `Object`. Object is synthesized by the compiler — there is no
source-level `class Object { ... }` declaration, and users do not write
`class Foo : Object` (that syntax is rejected).

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

**String value semantics**: Although string is a primitive type, calls to
`string.getHashCode()` and `string.equals(string)` are intrinsified to use
*value* semantics (`std::hash` for hash, content comparison for `equals`). This
makes strings usable as Dict keys without needing a wrapper
class.

**`==` operator is unaffected by `equals`**: Object.equals is an opt-in
method. The `==` operator on class references compares heap indices
directly. The reason
`equals` exists as a separate method is to allow user classes to override
with value equality without breaking identity-equality tests in the wider
codebase.

**Boxing (primitive → Object)**: A primitive value (int / float / string) is
implicitly boxed when assigned to an Object-typed target:

```nlang
Object o = 5;            // int boxed
Object f = 3.14;         // float boxed
Object s = "hi";         // string boxed

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 boxed at the call site
```

The runtime representation is a tagged boxed slot (slot[0] =
type tag, slot[1] = value bits). Boxed slots hold no references and are
explicitly skipped by the GC mark phase.

**`null` literal boxing preservation**: the literal `0` (used for `null`)
short-circuits OP_Box — no heap slot is allocated, and the value `0`
remains as the Object slot's contents. This keeps `Object o = null` and
`Object o = 0` as no-ops rather than wrapping 0 in a boxed-int heap ref.

**Unbox and class downcast (`as` operator)**:

```nlang
Object o = 5;
int x = o as int;          // explicit unbox — throws if boxed type ≠ int

Object f = 3.14;
int bad = f as int;        // throws: expected int, got float

class Point { public int x; }
Point p = new Point();
Object obj = p;
Point q = obj as Point;    // explicit class downcast — runtime-checked
Other o = obj as Other;    // throws: expected Other, got Point
```

The `as` keyword was chosen over C-style `(T)expr` prefix cast because
`(T)expr` cannot be reliably distinguished from parenthesized expressions
(the parser cannot tell `(foo) + bar` from `(foo + bar)`).
Keyword operators like `as` have no such ambiguity. This matches the
approach taken by C#, TypeScript, and Kotlin.

Supported conversions via `as`:
- Same type — no-op (e.g. same primitive type or same class)
- Boxing — primitive to Object (symmetric to the implicit-box path)
- Unboxing — Object to primitive (runtime tag check via `OP_Unbox`)
- Class downcast — Object to a subclass (runtime class check via
  `OP_CheckCast`, walks the heap slot's super chain)

Other conversions (e.g. `int as float`, `int as string`) are compile
errors — use the primitive cast / `ToString()` paths.

**Object upcast special-casing**: the implicitly inherited `Object`
does not appear in source-level inheritance declarations. The cast
checker special-cases
`target == Object` (any class upcast is a same-type no-op) and
`source == Object` (any class downcast is treated as a downcast) so that
`Object o = somePoint;` and `o as Point` work without requiring
the inheritance declaration to mention Object.

### Type Aliases

`using Name = Type;` declares a **type alias** — a shorthand for any
type expression, usable everywhere a type is expected:

```nlang
using Grid = Dict<string, List<int[]>>;
using Ints = int[];
using BinOp = Func<int, int>;

Grid g;                  // identical to the full type
List<Grid> lg;           // inside generic arguments
int apply(BinOp f) { ... }   // parameters and returns
```

**Semantics:**
- Aliases expand by a **pre-pass before name resolution**: each use site
  is replaced by a deep copy of the aliased type, so behavior is
  identical to writing the full type out.
- Scope is the **translation unit** — the same alias name may map to
  different types in different modules of one program.
- The right-hand side may **forward-reference** types declared later in
  the file (same as writing the type directly).
- An alias may reference **another alias**, but only one declared
  **earlier in the file** (textual order); a forward alias reference is
  a compile error.
- An alias name must not collide with classes, functions, other
  aliases, built-in type names (`List`, `Dict`, `Func`, `int`, ...),
  or a builtin package name (`math`, `io`, `fs`) in the same
  translation unit.
- The scope-opening form `using Foo;` (no `=`) is unchanged and
  unrelated.

**Restrictions:**
- The right-hand side must be a **primitive type name** (`int`, `float`,
  `string`, etc.), an **array type** (`int[]`), a **generic instantiation**
  (`List<int>`), or a **function type** (`Func<int, int>`). A bare
  **class/struct/enum type name** (`using X = Counter;`) is not a valid
  target — it fails as a parser syntax error. **Member paths**
  (`using X = ns.Inner;`) are also not supported — the grammar's type
  form is identifier-only, so they fail as a parser syntax error.
- Aliases are type-position only; a value expression can never resolve
  to an alias.
- Diagnostics anchored at a use site sometimes point at the `using` line
  (the expansion clone prefers the alias target's location).
