# Declarations


### Variable Declaration

```
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

```
enum Color { Red, Green, Blue }
enum Direction { North = 0, East = 90, South = 180, West = 270 }
```

Enum values are int32 at runtime. Members can be explicit or auto-incremented.

#### Enum Methods (Phase 12)

```
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
  serializes enum names only, not declarations) — this is a pre-existing
  limitation of the module format, not specific to methods.
- Arrays of enums: methods cannot be called on the array itself — index
  an element first (`a[i].rank()`, not `a.rank()`).

### Struct Declaration

```
struct Point {
    int x;
    int y;
}
```

Structs can contain:
- Primitive fields (int, float, string)
- Enum fields (stored as int32)
- Struct fields (deep-copied, owned by the containing struct)
- Class fields (reference, shallow-copied)

Structs cannot contain methods. Use classes for behavior.

### Class Declaration

```
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

**Inheritance layout**: Object memory layout is `[classIdx, ancestor_fields..., parent_fields..., own_fields...]`.
`classIdx` at slot[0] identifies the runtime class for virtual dispatch.

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

### Interface Declaration

```
interface IShape {
    public int Area();
    public int Perimeter();
}

class Square : IShape {
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
- `class X : IShape` (or `class X implements IShape`) — a class declares
  conformance with `:` or the `implements` keyword
- Virtual dispatch on interface-typed locals/params/fields
- Polymorphic collections (`List<IShape>` of mixed `Square`/`Circle`)

**Method `public` keyword is required** in interface declarations.
NLang's default access modifier is `private`; an interface method
declared as `int m();` (no `public`) is parsed but treated as private
and **not accessible** from call sites. The resulting error message
— "The function X does not exist or is not accessible" — is
misleading because the method does exist, just isn't public. Always
write `public int m();` in interfaces. (Java/C#-style "interface
members are inherently public" is a future language-design decision,
not current behavior.)

### Implicit `Object` Base Class

Every class that does not explicitly inherit from another class implicitly
inherits from `Object`. Object is synthesized by the compiler — there is no
source-level `class Object { ... }` declaration, and users do not write
`class Foo : Object` (that syntax is rejected).

Object provides two virtual methods with default identity semantics:

```
int Equals(Object other);    // identity: same heap reference → 1, else 0
int GetHashCode();           // identity: heap index of `this` (or 0 for null)
```

`Equals` and `GetHashCode` are virtual via name-based dispatch — subclasses
override them simply by declaring a method with the same name (no `override`
keyword needed; the runtime walks the class hierarchy and finds the
most-derived implementation first):

```
class Point {
    public int x;
    public int y;
    int GetHashCode() {              // overrides Object.GetHashCode
        return this.x * 31 + this.y;
    }
}
```

**String value semantics**: Although string is a primitive type, calls to
`string.getHashCode()` and `string.equals(string)` are intrinsified to use
*value* semantics (`std::hash` for hash, content comparison for Equals). This
makes strings usable as Dict keys in future phases without needing a wrapper
class.

**`==` operator unchanged**: Object.Equals is an opt-in method. The `==`
operator on class references continues to compare heap indices directly
(existing `class_null` / `class_virtual` tests do not regress). The reason
`Equals` exists as a separate method is to allow user classes to override
with value equality without breaking identity-equality tests in the wider
codebase.

**Boxing (primitive → Object)**: A primitive value (int / float / string) is
implicitly boxed when assigned to an Object-typed target:

```
Object o = 5;            // int boxed
Object f = 3.14;         // float boxed
Object s = "hi";         // string boxed

int TakesObject(Object o) { return o.getHashCode(); }
int x = TakesObject(42); // 42 boxed at the call site
```

The runtime representation is a tagged slot of kind `RTK_Boxed` (slot[0] =
type tag, slot[1] = value bits). Boxed slots hold no references and are
explicitly skipped by GC MarkPhase.

**`null` literal boxing preservation**: the literal `0` (used for `null`)
short-circuits OP_Box — no heap slot is allocated, and the value `0`
remains as the Object slot's contents. This keeps `Object o = null` and
`Object o = 0` as no-ops rather than wrapping 0 in a boxed-int heap ref.

**Unbox and class downcast (`as` operator)** — Phase 8e-1.5:

```
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
`(T)expr` introduces LALR(1) conflicts with parenthesized expressions
(the parser cannot disambiguate `(foo) + bar` from `(foo + bar)`).
Keyword operators like `as` have no such ambiguity. This matches the
approach taken by C#, TypeScript, and Kotlin.

Supported conversions via `as`:
- `TCK_Same` — no-op (e.g. same primitive type or same class)
- `TCK_Box` — primitive to Object (symmetric to the implicit-box path)
- `TCK_Unbox` — Object to primitive (runtime tag check via OP_Unbox)
- `TCK_Downcast` — Object to a subclass (runtime class check via
  OP_CheckCast, walks the heap slot's super chain)

Other conversions (e.g. `int as float`, `int as string`) are compile
errors — use the existing primitive cast / `ToString()` paths.

**Object upcast special-casing**: AST-level `SnClassDecl::SuperClass()`
does not include the implicit Object parent (only VmBackend's
`CompiledClass.superClassIdx` does). The cast checker special-cases
`target == Object` (any class upcast is TCK_Same, no-op) and
`source == Object` (any class downcast is TCK_Downcast) so that
`Object o = somePoint;` and `o as Point` work without requiring
Point's AST parent chain to mention Object.

### Type Aliases (Phase 13)

`using Name = Type;` declares a **type alias** — a shorthand for any
type expression, usable everywhere a type is expected:

```
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
  or the reserved stdlib namespaces (`math`, `io`, `fs`) in the same
  translation unit.
- The namespace-opening form `using Foo;` (no `=`) is unchanged and
  unrelated.

**Restrictions:**
- The right-hand side must be a plain type form (type name, generic
  instantiation, array suffix). **Member paths** (`using X = ns.Inner;`)
  are not supported — the grammar's type form is identifier-only, so
  they fail as a parser syntax error.
- Aliases are type-position only; a value expression can never resolve
  to an alias.
- Diagnostics anchored at a use site sometimes point at the `using` line
  (the expansion clone prefers the alias target's location).
