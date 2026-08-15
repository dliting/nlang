# NLang Language Specification

## Overview

NLang is a statically-typed scripting language with C-like syntax. It
compiles to bytecode executed by a register-based VM. Originally inspired
by the EN engine's `compiler_bak`/`lang_bak`, it now evolves as an
independent language.

Key design goals:
- Familiar C-family syntax for low learning curve
- Value semantics for structs, reference semantics for classes (like C#)
- Deterministic, inspectable memory layout (heap slots, typed kinds)
- Mark-sweep garbage collection for class objects
- Embeddable in host applications via a small C++ API

## Naming Convention (Phase 8e-9-pre)

NLang adopts **camelCase** for methods and **PascalCase** for types — a hybrid
of Java-style casing and C#-style accessor naming. This combination provides
visual distinction between types and methods (`MyClass.myMethod()` reads
unambiguously) and aligns with the largest developer audience (Java + JS + C++).

| Category | Style | Examples |
|----------|-------|----------|
| Types (class / struct / enum / interface) | PascalCase | `MyClass`, `List<T>`, `Color` |
| Methods — action / command (side effects or multi-arg) | camelCase, bare verb | `add(x)`, `clear()`, `readInt()`, `run()` |
| Methods — pure accessor (no side effects, no args, returns value) | camelCase with `get`/`set` prefix | `getHashCode()` (reserved for future property feature) |
| Methods — predicate (returns bool) | camelCase, bare word | `equals(o)`, `contains(x)` |
| Free functions | camelCase | `print(s)`, `assert(c)` |
| Variables / parameters / locals | camelCase | `firstName`, `itemCount` |
| Entry-point function `main` | lowercase (sole exception) | `int main()` |
| Enum values | PascalCase | `Color.Red`, `Day.Monday` |
| Generic type parameters | single uppercase letter | `T`, `K`, `V` |
| Private fields | camelCase, no prefix | `class Foo { int count; }` |

**Rationale**:
- PascalCase types + camelCase methods → `MyClass.myMethod()` makes the
  type-vs-method distinction immediate; `MyClass.MyMethod()` is ambiguous.
- Covers Java + JS + C++ conventions (the largest common denominator).
- `main` exception preserves the universal C/C++/Java entry-point convention.
- `getXxx` / `setXxx` prefix retained on accessors: reserves namespace for a
  future property feature (`obj.hashCode` desugaring to `getHashCode()` /
  `setHashCode(v)`). Only `getHashCode` currently uses this form; other
  accessors (`length`, `count`, `position`, `keys`) use bare camelCase and
  may be upgraded to `getXxx` when properties land.
- Predicates do not use `isXxx` / `hasXxx` prefixes — `equals` and
  `contains` are clear on their own.

**Built-in method migration** (Phase 8e-9-pre): all built-ins renamed from
PascalCase to camelCase. Notable: `Length→length`, `Add→add`, `Equals→equals`,
`GetHashCode→getHashCode`, `ReadInt→readInt`, `WriteString→writeString`,
`Keys→keys`, `ContainsKey→containsKey`.

## Types

### Primitive Types

| Type   | Size  | Description          |
|--------|-------|----------------------|
| int    | 4 bytes | 32-bit signed integer |
| float  | 4 bytes | 32-bit IEEE 754 float |
| string | 4 bytes | Reference to string pool entry |

**String encoding**: String literals are stored as their UTF-8 byte sequence
in the module string pool. `string.length()` returns the **byte count**, not
the Unicode code-point count — `"héllo".length()` is 6 (5 code points but `é`
is 2 bytes in UTF-8). Proper UTF-8 code-point iteration is deferred to a
future phase.

### Composite Types

| Type   | Semantics | Storage          | Description              |
|--------|-----------|------------------|--------------------------|
| enum   | Value     | int32            | Named integer constants  |
| struct | Value     | Heap index (copy-on-assign) | Value-typed aggregate |
| class  | Reference | Heap index       | Reference-typed object   |

### Null

Class-typed variables can be null (represented as heap index 0). Accessing
fields or methods on null throws a `NullPointerException` (catchable via
try/catch since Phase 9d).

## Type Semantics

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
```
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

## Declarations

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

## Built-in Generic Classes

### `List<T>` — Phase 8e-3

`List<T>` is a growable, ordered, index-addressable collection. It is
a **built-in generic class** — only `List` (and future `Dict`) are
recognized by the compiler; user-defined `class Foo<T>` is not (yet)
supported.

```
List<int> nums = new List<int>();
nums.add(1);
nums.add(2);
nums.add(3);
int sum = nums.get(0) + nums.get(1) + nums.get(2);    // 6
int n = nums.length();                                 // 3

List<string> names = new List<string>();
names.add("alice");
names.add("bob");
int total = (names.get(0) + names.get(1)).length();    // 8
```

**Methods** (T is the element type):

| Method              | Signature          | Returns | Notes                              |
|---------------------|--------------------|---------|------------------------------------|
| `Add`               | `void Add(T item)` | —       | Append to end                      |
| `Get`               | `T Get(int idx)`   | T       | Read by index; throws if OOB       |
| `Set`               | `void Set(int i, T)` | —     | Overwrite element                  |
| `Length`            | `int Length()`     | int     | Current element count              |
| `RemoveAt`          | `void RemoveAt(int i)` | —   | Erase; shifts later elements down  |
| `IndexOf`           | `int IndexOf(T item)` | int | First index of `item`, or -1       |
| `Contains`          | `int Contains(T item)` | int | 1 if present else 0               |
| `Clear`             | `void Clear()`     | —       | Remove all elements                |

**Type checking**: the compiler recognizes `List<int>`, `List<string>`,
`List<Point>`, etc. as distinct static types. Argument types are checked
against the substituted signature — `nums.add("wrong")` is a compile
error when `nums : List<int>`.

**Erasure runtime model**: `List<int>` and `List<Point>` share the same
backing class at runtime. Elements are stored uniformly as heap indices
in a side table (`m_listStore`); primitive elements are boxed via
`OP_Box` at the call site. GC traces list elements as additional roots.

**Null List reference**: a `List<T>` field or variable that has not been
assigned `new List<T>()` holds null. Calling any method on null throws
`null reference in CallMethod` (same NPE semantics as other class refs).

**Collection initializer**: `[1, 2, 3]` literal syntax is supported
since Phase 8e-6 (bare bracket form for arrays and `List<T>`). See the
Collection Initializers section above.

**`foreach`**: the `foreach (Type var in iterable)` construct is supported
since Phase 8e-5. See the Foreach Statement section below.

**Nested generics** (`List<List<int>>`): the lexer tokenizes `>>` as a
single `OT_RSH` (right-shift) token, which blocks nested generic type
args. This is a known limitation; use `new List<T>{...}` as the outer
wrapper or split into local variables.

### `Dict<K,V>` — Phase 8e-4

`Dict<K,V>` is an associative array mapping keys of type `K` to values
of type `V`. Like `List<T>`, it is a **built-in generic class** — only
`List` and `Dict` are recognized by the compiler; user-defined generics
are not (yet) supported.

```
Dict<string,int> scores = new Dict<string,int>();
scores.set("alice", 90);
scores.set("bob",   85);
int a = scores.get("alice");          // 90
int hasBob = scores.containsKey("bob"); // 1
int n = scores.count();                 // 2

Dict<int,int> squares = new Dict<int,int>();
squares.set(3, 9);
squares.set(4, 16);
squares.set(3, 99);                     // overwrites 9 → 99
int v = squares.get(3);                 // 99
int removed = squares.remove(4);        // 1
```

**Methods** (K is the key type, V is the value type):

| Method           | Signature                  | Returns | Notes                                          |
|------------------|----------------------------|---------|------------------------------------------------|
| `Set`            | `void Set(K key, V value)` | —       | Insert-or-replace (no duplicate-key error)     |
| `Get`            | `V Get(K key)`             | V       | Lookup; **throws** if key absent               |
| `ContainsKey`    | `int ContainsKey(K key)`   | int     | 1 if present, 0 otherwise                      |
| `Remove`         | `int Remove(K key)`        | int     | 1 if removed, 0 if key not found               |
| `Clear`          | `void Clear()`             | —       | Remove all entries                             |
| `Count`          | `int Count()`              | int     | Current entry count                            |

**Type checking**: the compiler recognizes `Dict<int,int>`,
`Dict<string,Point>`, etc. as distinct static types. Argument types are
checked against the substituted signature — `d.set("x", "y")` is a
compile error when `d : Dict<string,int>`.

**Erasure runtime model**: `Dict<K,V>` shares a single backing class
across all instantiations. Entries are stored as `(K heap idx, V heap
idx)` pairs in a side table (`m_dictStore`); primitive keys/values are
boxed via `OP_Box` at the call site. GC traces every entry's K and V as
additional roots.

**Key equality** is kind-aware:
- Primitive keys (boxed `int`, `float`): compare value bits (IEEE 754 —
  `NaN != NaN`, documented behavior).
- `string` keys: compare string-pool content (value equality).
- `class` / `struct` keys: compare heap idx (identity), matching Java's
  `IdentityHashMap` and C#'s default `object.Equals`. A user `Equals`
  override is **not** consulted — override-based dictionary semantics
  are a separate future phase.

**Null Dict reference**: a `Dict<K,V>` field or variable that has not
been assigned `new Dict<K,V>()` holds null. Calling any method on null
throws `NLang VM: Dict <method> on null instance`.

**Linear-scan lookup (current limitation)**: every `Set`/`Get`/
`ContainsKey`/`Remove` does an O(n) scan of the entries vector. This is
acceptable for typical small scripts; O(1) hashtable lookup is a future
optimization phase.

**`foreach` over keys (Phase 8e-5)**: `foreach (K k in dict) { ... }`
iterates the keys of the dict, Python/JavaScript style. Inside the body,
call `dict.get(k)` to access the value. Implementation: codegen emits
an inline `dict.keys()` call to materialize a fresh `List<K>`, then
iterates that list. See the Foreach Statement section below.

**`Dict.keys()`**: returns a new `List<K>` populated with all keys
(no defined ordering). Useful independently of `foreach` for snapshotting
keys for enumeration, set-style membership checks via `Contains`, etc.
The returned `List<K>` is a *copy* — subsequent `Set`/`Remove` on the
source dict do not affect it.

**`Values()`**: not yet provided. Iterate keys and call `Get` to obtain
values.

## Expressions

### Arithmetic

```
a + b    a - b    a * b    a / b    a % b
```

Integer division truncates toward zero. Division/modulo by zero throws a
runtime error — this includes `float` division by zero, which throws
rather than producing IEEE 754 ±inf/NaN (NLang diverges from C/C++/Java
here).

**Integer overflow** wraps silently in two's complement (C-style):
`INT_MAX + 1 == INT_MIN`. There is no SafeInt-style checking. Lock-in
test: `tests/e2e/int_overflow_wrap.n`.

**Numeric promotion (Phase 8e-8)**: arithmetic ops follow symmetric C-style
promotion — both operands are promoted to the wider type before the op:
- `int + int` → int
- `int + float` / `float + int` → float (both operands promoted to float)
- `float + float` → float

So `1 + 2.5 == 2.5 + 1 == 3.5` (symmetric). The result type is the promoted
type; assignment to a narrower type (e.g. `int r = 1.5 + 1;`) implicitly
truncates.

`string + string` (OP_Add only) is concatenation. `string - string` etc.
are compile errors.

### Comparison

```
a == b   a != b   a < b   a > b   a <= b   a >= b
```

Returns 1 (true) or 0 (false). String equality compares content. String
relational ordering (`<`, `>`, `<=`, `>=`) uses C `strcmp`-style byte-by-byte
ASCII comparison (e.g. `"Z" < "a"` is true because `'Z'` (90) < `'a'` (97)).

### Logical

```
a && b   a || b   !a
```

Non-zero is truthy. Both operands are always evaluated — there is **no
short-circuit**. `false && expr` and `true || expr` will still evaluate
`expr` (including any side effects or throws it triggers). This differs
from C/C++/Java/Python which all short-circuit.

### Member Access

```
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check).

### Object Creation

```
Node n = new Node();
Node n = new Node(42);
```

Allocates on the heap, calls constructor if present.

### Type Casts

```
int x = 5;
float y = (float)x;
int z = (int)y;
```

Explicit casts between int and float. Implicit widening (int→float) is
allowed in some contexts.

### Primitive → String Coercion (Phase 8e-9a)

When a primitive (int or float) appears in a context expecting string,
NLang auto-coerces it to its decimal string form. This is most common in
string concatenation, but also fires in direct assignment and field stores.

```
string s1 = "x" + 5;       // "x5" — int coerced to "5"
string s2 = 5 + "x";       // "5x" — symmetric
string s3 = "x=" + 2.5;    // "x=2.5" — float uses %g format
string s4 = "a" + 1 + "b" + 2.5 + "c";  // "a1b2.5c"
string s5 = 42;            // "42" — direct assignment path
string s6 = "x" + (-7);    // "x-7" — negative formatted with sign
```

**Implementation**:
- `int → string`: `OP_Int32_to_str` (decimal, via `std::to_string`)
- `float → string`: `OP_Float_to_str` (`%g` format — `2.5` not `2.500000`)
- Both push the formatted string into the runtime `m_stringPool` and write
  the new index back to `pResult`. Followed by `OP_Assign` to move into the
  destination slot.
- `string → int/float` remains rejected (`TCK_None` in CastInfo.cpp) — use
  `int.parse(s)` style helpers when standard library lands.

**Object.toString() Protocol** (Phase 8e-9b):

All class instances inherit `string toString()` from `Object`. The default
implementation returns `"ClassName@heapIdxHex"` (e.g. `"Point@7"`, `"Point@ff"`).
User classes override it by declaring `string toString() { ... }` — virtual
dispatch by name, same as `equals`/`getHashCode`.

```
class Point {
    public int x;
    public int y;
    string toString() { return "P(" + this.x + "," + this.y + ")"; }
}
```

Dispatch matrix:

| Receiver | `.toString()` result | Override? |
|----------|---------------------|-----------|
| class (user override) | user-defined | yes |
| class (no override) | `"ClassName@hex(heapIdx)"` | no (Object intrinsic) |
| enum | enum member name (e.g. `"Red"`) | no |
| int | decimal string (e.g. `"42"`) | no |
| float | `%g` format (e.g. `"2.5"`) | no |
| string | self (identity) | no |

**Implicit coercion**: `"x" + obj` automatically calls `obj.toString()`, same
as Java/C#. This applies to class, enum, int, and float receivers. Struct
receivers are **permanently excluded** — `"x" + structInstance` is a compile
error (struct is a pure-data type in NLang; use class for object semantics).

**Enum name output**: `Color.Red.toString()` returns `"Red"` (not `"0"`). The
compiler embeds a per-enum name table; the VM uses `OP_Enum_to_str` to look up
the member name by value. Out-of-range enum values throw at runtime.

**String identity**: `"hello".toString()` returns `"hello"` — the resolver folds
this to a no-op (no opcode emitted).

**Limitations** (deferred to future phases):
- No warning when implicit coercion occurs (silent, like Java)
- `struct.toString()` / `"x" + structInstance` — permanently rejected

**Escape sequences** (inside double-quoted literals):

| Escape      | Produces              |
|-------------|-----------------------|
| `\n` `\r` `\t` | newline, CR, tab   |
| `\\` `\"` `\'` | backslash, quote, apostrophe |
| `\0` `\a` `\b` `\f` `\v` | NUL, bell, backspace, form feed, vertical tab |
| `\x`/`\u`... | not supported        |

Any other escape (e.g. `\q`) is a compile error — escapes never pass
through as literal backslash pairs. Interpolation and escapes compose:
`"${name}\n"` interpolates then appends a newline.


### String Interpolation (Phase 9b)

```
string name = "world";
string s = "Hello ${name}!";   // "Hello world!"
```

NLang supports `${identifier}` interpolation inside double-quoted string
literals — the named variable's value is rendered via the same coercion
paths as Phase 8e-9a (primitive → string) and Phase 9b-pre (collection
`toString()`). The interpolation is implemented by scanning the literal
content in the bison `TT_String` rule and constructing an `OP_Add` binary
tree; no new opcode, resolver method, or codegen handler is introduced.

**Syntax constraints** (MVP):

- Only a single identifier is supported inside `${...}`. Complex
  expressions like `${a + b}`, `${obj.method()}`, or `${this.x}` are
  rejected at parse time. Use a separate variable: `int sum = a + b;
  "result=${sum}"`.
- `${name}` where `name` is not in scope produces a compile error ("undefined
  identifier") at the resolver stage — the same path as any other undefined
  identifier reference.
- Empty `${}` and invalid identifier contents (e.g. `${123}`, `${a b}`)
  produce a compile error.

**Dollar escape**: `$$` produces a literal `$` in the resulting string.
`$${name}` produces the literal text `${name}` (no interpolation). A lone
`$` not followed by `$` or `{` is preserved as a literal `$`.

```nlang
string name = "x";
string a = "$${name}";  // literal "${name}"
string b = "price: $";  // literal "price: $"
string c = "$$100";     // literal "$100"
```

**Type dispatch**: the identifier's resolved type determines the coercion
applied automatically:

| Identifier type | Coercion applied | Phase |
|-----------------|------------------|-------|
| `int` | `OP_Int32_to_str` | 8e-9a |
| `float` | `OP_Float_to_str` | 8e-9a |
| `string` | none | — |
| `enum` | `OP_Enum_to_str` | 8e-9b |
| `Array` | `OP_Array_to_str` | 9b-pre |
| `List` / `Dict` | `OP_CallMethod "toString"` | 9b-pre |
| `class` | `OP_CallMethod "toString"` | 8e-9b |

### Runtime-checked Cast (`as`)

```
expr as TypeName
```

Runtime-checked conversions, supported in Phase 8e-1.5:

- **Unbox**: `o as int` / `o as float` / `o as string` — unwrap a boxed
  primitive. Throws if `o` is null or the boxed type tag doesn't match.
- **Class downcast**: `o as SubClass` — verify the runtime class of `o`
  is `SubClass` or a subclass thereof. Throws on mismatch.
- **Identity / upcast**: `o as Object` — no-op (any class is already
  Object). Allowed for symmetry.

Type-incompatible casts (`5 as string`, `o as int` when `o` holds a
class ref) are compile errors — `as` only permits same/box/unbox/downcast.

### Collection Initializers (Phase 8e-6)

NLang supports C-style collection literals for arrays, lists, dicts,
and aggregate (struct/class) initialization. Two syntactic forms:

**Bare bracket form `[...]`** — allowed only where the LHS or assignment
target lets the resolver infer the collection type. Works for arrays
(`T[]`) and `List<T>`:

```
int[] arr = [1, 2, 3];
string[] names = ["alice", "bob"];
List<int> nums = [10, 20, 30];
List<Point> pts = [new Point{x:1, y:2}, new Point{x:3, y:4}];
```

**Explicit form `new Type{...}`** — works in any expression position
(function args, return values, standalone expressions). Required for
dict, struct, and class initialization because bare `{...}` would
conflict with the `Paragraph` (block statement) grammar:

```
Dict<string, int> d = new Dict<string, int>{"a":1, "b":2};
Point p = new Point{x:1, y:2};
List<int> lst = new List<int>{1, 2, 3};
return new Point{x:0, y:0};
foo(new Point{x:1, y:2}, new Point{x:3, y:4});
```

**Entry forms inside `{...}`:**

- `TT_String : Expression` — dict entry (string key)
- `TT_Identifier : Expression` — struct/class field (e.g. `x:1, y:2`)
- `Expression` (no key) — list element (only valid when Type is `List<T>`)

**Type disambiguation:** the resolver uses the LHS variable (or the
explicit `Type` in `new Type{...}`) to pick the kind:

| Target type          | Form    | Entry kind              |
|----------------------|---------|-------------------------|
| `T[]` (array)        | `[...]` | value-only              |
| `List<T>`            | `[...]` or `new List<T>{...}` | value-only |
| `Dict<K,V>`          | `new Dict<K,V>{...}` | `key : value` (string key) |
| struct               | `new StructName{...}` | `field : value` (identifier key) |
| class                | `new ClassName{...}` | `field : value` (identifier key) |

**Class init requirements:** the class must have a no-arg constructor
(explicit or implicit). Codegen lowers `new C{f1:v1, ...}` as
`new C()` followed by per-field `OP_StoreField` assignments.

**Recursive nesting:** init lists may contain other init lists, but
nested generics like `List<List<int>>` and `Dict<K, List<V>>` are
blocked by the lexer (tokenizes `>>` as right-shift). Use `new List<T>{...}`
as the outer wrapper where needed, or split into local variables.

**Empty collections:** bare `[]` is not supported (the lexer matches
`[]` as a single `OT_Brackets` token used for array-type suffix). Use
the explicit empty form instead: `new List<T>{}`, `new Dict<K,V>{}`,
or `new int[0]` for arrays.

**Function arg disambiguation:** a bare `[...]` as a function argument
is not currently supported — it produces a compile error because the
resolver cannot infer the target type without overload resolution.
Use the explicit `new Type{...}` form for function args (Phase 8e-6
Phase G — overload uniqueness — is deferred).

**Mutation during init is UB.** Entries are evaluated left-to-right and
assigned in order; reading the partially-constructed collection from
within an entry expression (e.g. `[1, foo(arr)]` where `foo` reads
`arr`) is undefined behavior. An exception-throwing entry leaves the
collection partially constructed.

## Statements

### Control Flow

```
if (cond) { ... }
if (cond) { ... } else { ... }

while (cond) { ... }
do { ... } while (cond);
for (init; cond; fini) { ... }
foreach (Type var in iterable) { ... }

break;
continue;
return;
return expr;
```

**Condition typing:** `if`/`while`/`do-while`/`for`/`assert` conditions
must be `int` (comparisons produce `int`). String, float, class, struct,
and array conditions are compile errors — the VM's `OP_JumpIfNot` reads
a single int32, and non-int values (string pool handles, heap indices)
have no meaningful truthiness. Use an explicit comparison instead:
`if (s != "")`, `if (obj != null)`.

### Foreach Statement (Phase 8e-5)

```
foreach (Type var in iterable) { body }
```

Iterates the elements of `iterable`, binding each to `var` for the body.
Supported iterables:

| Iterable | Iterates | Element access |
|----------|----------|----------------|
| `T[N]` (array) | elements `arr[0]..arr[N-1]` | `OP_LoadElement` |
| `List<T>` | elements in insertion order | `List<T>.get(i)` |
| `Dict<K,V>` | **keys** (Python style) | inline `dict.keys()` then `List<K>.get(i)` |

`break` and `continue` work identically to `for`. The loop variable is
**function-scoped** (NLang has no block scope, consistent with `for`):

```
List<int> nums = new List<int>();
nums.add(10); nums.add(20); nums.add(30);
int sum = 0;
foreach (int x in nums) {
    sum = sum + x;
}
// `x` remains in scope here (function-scoped)
```

**Dict iteration example**:

```
Dict<string, int> ages = new Dict<string, int>();
ages.set("alice", 30);
ages.set("bob",   25);
int total = 0;
foreach (string name in ages) {
    total = total + ages.get(name);
}
// total == 55
```

**Mutation is undefined behavior**. The element count is cached at loop
entry (`n = iterable.length()` for List/Dict, `n = arr.length` for Array).
Structural modifications inside the body (`List.Add`/`RemoveAt`,
`Dict.Set`/`Remove`) may cause: out-of-bounds access, skipped/duplicated
elements, or stale `Keys()` snapshots. Element assignment (`arr[i] = x`)
inside an Array foreach body is fine (no structural change).

**Struct elements are copied into the loop variable** (value semantics):
`foreach (Point p in arr) { p.x = 99; }` does not modify `arr`'s elements —
`p` is a fresh deep copy per iteration (consistent with C#, where foreach
over value-type elements also yields copies).

**Null iterable** throws NPE on the first `length()`/`Length()` call
(consistent with all other class-typed calls).

**`List<int>` with value 0**: due to a pre-existing `OP_Box` optimization
(literal `0` is treated as the null sentinel), `foreach` over a `List<int>`
containing literal-zero elements currently throws `unbox on null/invalid
reference`. This is a boxing limitation, not a `foreach` bug — work around
by avoiding 0 as a list element value. A future phase will revisit the
null-sentinel design.

### Compound Assignment (Phase 9a)

```
x += y ;  x -= y ;  x *= y ;  x /= y ;  x %= y ;
```

Read-modify-write shorthand for `x = x op y`. Supported left-values:
local variables, class fields (`this.f += y`), struct fields (`pt.x += y`).
The left value is evaluated **only once** (so `obj.something() += 1` would
not double-invoke `something()`).

Not supported: subscript left-value (`arr[i] += 1`). The bytecode frame
layout doesn't have enough scratch slots for single-evaluation of
subscript read-modify-write. Use the explicit form `arr[i] = arr[i] + 1`.

### Assert Statement (Phase 9a)

```
assert(condition);
```

Evaluates `condition`. If false, throws an `AssertionException` which can be
caught by a `try/catch` block (Phase 9d). If uncaught, terminates the program
with exit code 1. Single-argument form only (no message override yet).

### Exception Handling (Phase 9d)

NLang supports structured exception handling with a Java/C#-style class
hierarchy. All exceptions are instances of `Exception` or its subclasses.

**Built-in exception classes:**

| Class | Superclass | Thrown by |
|-------|-----------|-----------|
| `Exception` | `Object` | User `throw` / Dict key not found |
| `NullPointerException` | `Exception` | Null reference access |
| `DivByZeroException` | `Exception` | Integer division/modulo by zero |
| `IndexOutOfBoundsException` | `Exception` | Array/List index out of bounds |
| `AssertionException` | `Exception` | `assert(false)` |

**try/catch:**

```
try {
    // code that may throw
} catch (DivByZeroException e) {
    // handle division by zero
} catch (Exception e) {
    // handle any other exception
}
```

- Multiple `catch` clauses are supported, matched in declaration order.
- The first matching catch clause executes; subsequent ones are skipped.
- `catch (Exception e)` catches all exceptions (Exception is the base class).
- The catch variable `e` is a regular local variable within the catch body.

**throw:**

```
throw new Exception("error message");   // throw a new exception
throw;                                   // re-throw current exception (only inside catch)
```

- `throw expr` — the expression must evaluate to an Exception subclass instance.
  Throwing a non-Exception value (e.g. `throw 42`) is a compile error.
- `throw;` (re-throw) is only valid lexically inside a `catch` body. Using it
  outside a catch block is a compile error.

**User-defined exception subclasses:**

```
class MyException : Exception {
    int code;
    public int MyException(string msg) {
        super(msg);          // forward to Exception(message) ctor
        this.code = 42;
        return 0;
    }
}
```

User classes can extend `Exception` to carry additional fields. Without a
user constructor, the default constructor is used and inherited fields are
zero-initialized; a user ctor typically forwards the message via
`super(msg)` (see "super() — constructor chaining" below).

**Exception fields:**

Exception instances expose two readable/writable fields:

- `message` (string) — the exception message. Set by the constructor
  (`new Exception("msg")`) or by VM error sites. Writable by user code.
- `backtrace` (List&lt;string&gt;) — call stack snapshot at throw time for
  VM-thrown exceptions (`funcName.n:line` entries, innermost first).
  User-constructed exceptions start with an empty backtrace.

```
try {
    int x = 0;
    int y = 1 / x;
} catch (DivByZeroException e) {
    int n = e.message.length();       // > 0 — VM sets the message
    int frames = e.backtrace.length(); // >= 1 — VM snapshots the stack
}

//User subclasses inherit both fields; own fields land after them.
class MyException : Exception {
    int code;
}
MyException e = new MyException();
e.message = "custom";   // writable
e.code = 42;

**VM errors are catchable:**

Runtime errors that previously caused hard crashes (NPE, division by zero,
array/list index out of bounds, assertion failure) now throw the corresponding
Exception subclass and can be caught:

```
try {
    int x = 0;
    int y = 1 / x;           // throws DivByZeroException
} catch (DivByZeroException e) {
    // caught
}

try {
    List<int> lst = new List<int>();
    return lst.get(999);      // throws IndexOutOfBoundsException
} catch (IndexOutOfBoundsException e) {
    // caught
}
```

**Uncaught exceptions** propagate up the call stack. If no handler is found,
the program terminates with exit code 1 (same as the pre-9d behavior).

**finally (Phase 9d-2):**

```
try {
    riskyWork();
} catch (Exception e) {
    handle(e);
} finally {
    cleanup();     // always runs
}
```

`finally` has full Java semantics — the finally body runs when control
leaves the try region by **any** of these paths:

- try body completes normally (including falling out the bottom)
- a catch clause completes (matched or not)
- an exception unwinds through (the finally handler runs its body copy,
  then re-throws the original exception; this includes exceptions thrown
  from inside a catch body)
- `break` / `continue` transfer out of the region (an inline copy of the
  finally body runs before the jump, innermost-first for nested tries)
- `return` (the return expression is evaluated **first**, then the finally
  body runs, then the function returns)

Nesting: inner finally bodies run before outer ones; after them, the
surrounding catch (if any) sees the exception. Each finally body executes
exactly once per control-flow pass — the normal-path and exception-path
copies are disjoint code regions.

`try { } finally { }` without any catch clause is legal (the finally entry
is the only handler).

**Restriction**: `break`, `continue`, `return`, and `throw` are not
allowed *inside a finally body* (compile error). A finally body must not
swallow the in-flight control flow or exception.

**super() — constructor chaining (Phase 9d-2):**

```
class Base {
    public int v;
    public int Base(int x) { this.v = x; return 0; }
}
class Kid : Base {
    public int Kid(int x) {
        super(x * 2);        // calls Base(int)
        return 0;
    }
}
```

- `super(args);` invokes the **direct parent class's constructor** on the
  same `this` object. Valid only inside a constructor of a class that has
  a parent (`Object` has none).
- May appear at **any statement position** in the ctor (not restricted to
  the first statement).
- Argument count must match the parent ctor's parameter count. For parents
  in the built-in Exception family the ctor takes exactly one `message`
  argument.
- Named arguments (`super(x = 1)`) are not supported (compile error).
- `super()` with no arguments against a parent with no constructor is a
  legal no-op; passing arguments in that case is a compile error.

### Const Local Variables (Phase 9a)

```
const int X = 5;
const string Greeting = "hello";
```

Local variables marked `const` must be initialized at declaration and
cannot subsequently be assigned or compound-assigned. Only **local**
const is supported; class/struct field const is not (constructor
initialization order would add complexity, deferred to a future phase).

**Const is shallow (Java-`final`-style)**: `const` prevents rebinding the
*name* but does not freeze the referenced object's state. Member mutation
through a const local is allowed:

```
const Foo f = new Foo();
f.x = 5;            // OK — f itself is not reassigned
f = new Foo();      // ERROR — cannot reassign const local
const Point p = q;
p.x = 5;            // OK — only p's binding is const
```

For class fields and array elements this means: a `const` reference still
permits writing through it. Deep/immutability-style const is intentionally
out of scope for Phase 9a and may be revisited in a future phase.

### Switch

```
switch (value) {
    case 1: ...
    case 2: ...
    default: ...
}
```

Switch values are int32 (including enum values). Break exits the switch.

**No fall-through.** Each case body ends with an implicit break — execution
does not cascade into the next case body even without an explicit `break`
statement. This matches Java/C# semantics, not C/C++. (The codegen emits a
per-case equality check; after a case body runs, control flows into the next
case's comparison, which skips its body because the value no longer matches.
Distinct case values therefore trigger exactly one body.) The `break` keyword
is only needed to exit early from inside a multi-statement case body or to
break out of an enclosing loop.

### Null Check

Accessing a field or method on a null class reference throws a
`NullPointerException` (Phase 9d), which can be caught by a `try/catch`
block. If uncaught, the program terminates with exit code 1.

## Functions

```
int add(int a, int b) {
    return a + b;
}
```

- Parameters: int/float/string/enum passed by value, struct passed by value
  (deep copy), class passed by reference
- Return type: int, float, string, enum, struct (deep copy), class (reference)
- Recursion: supported, with a depth limit (default 1000)

### Default Parameters (Phase 9c)

Function parameters may have default values. Defaults can appear at any
position (not just trailing). A default expression may reference earlier
formal parameters.

```
int foo(int a, int b = 0) { return a + b; }
int foo(int a, int b = a + 1) { return b; }       // references earlier param
int foo(int a, int b = 0, int c) { return c; }     // default not at end
```

- `foo(5)` → `b` gets default value
- `foo(5, 10)` → `b` is 10, default not evaluated
- Default expressions are evaluated at the call site (not at declaration)
- Type mismatch between default expression and parameter type is a compile error

### Named Arguments (Phase 9c)

Arguments may be passed by name using `name = expr` syntax. Positional
arguments must precede named arguments.

```
int foo(int a, int b) { return a * 10 + b; }
foo(a = 5, b = 7);     // named, any order
foo(5, b = 7);          // mixed: positional then named
foo(b = 7, a = 5);      // named, reversed order
```

Errors:
- `foo(b = 2, 1)` — positional after named: compile error
- `foo(1, a = 2)` — duplicate binding for `a`: compile error
- `foo(c = 1)` — unknown parameter name: compile error

### Overload Resolution with Defaults (Phase 9c)

When multiple overloads exist, the compiler selects the best match by
computing a type-distance score. If two or more overloads match with equal
distance, the call is ambiguous and a compile error is reported.

```
int foo(int a) { return 100; }
int foo(int a, int b = 0) { return 200; }
foo(5, 10);   // OK: second overload (2 args match 2 formals)
foo(5);       // Error: ambiguous (both overloads accept 1 arg)
```

### Frame Layout (Phase 9c follow-up)

Each function's local frame is sized dynamically based on its body:

```
[this?][params][returnSlot][temp1-4][callParamBase(N)][evalArea(peakDepth)][user locals...]
```

- **N** = max callee formal count (plus slot 0 for `this` on methods) observed
  in this function's body. `callParamBase` is the final landing zone consumed
  by `OP_CallFunc`/`OP_CallMethod`.
- **peakDepth** = max simultaneous evalArea slot need across all call sites,
  including nested calls (e.g. `foo(helper(5), helper(10))` needs 4 slots:
  2 for `foo`'s args + 2 for the inner `helper` calls).

The `evalArea` is a disjoint, stack-disciplined staging area. Each
`EmitCallArgs` invocation claims a slice via the `EvalAreaClaim` RAII guard
on entry and releases on exit. Bindings emit to the claimed slice; a
bulk-copy loop then moves them to `callParamBase` just before the call.
This means inner calls' bindings never overwrite outer calls' already-emitted
bindings — fixing the pre-existing `callParamBase` nested-call clobber bug.

A sanity ceiling of 64 formals (`kMaxFuncParams`) prevents unreasonably
large frames; exceeding it is a declaration-time error.

## Memory Management

### Heap Layout

All struct and class objects share a single heap (`m_structHeap`). Each heap
slot is a `vector<int32>`:

- **Struct slot**: `[field0, field1, ...]` (no type header)
- **Class slot**: `[classIdx, field0, field1, ...]` (slot[0] = runtime type ID)

Heap index 0 is a sentinel (null/invalid). Valid indices start at 1.

### Garbage Collection

NLang uses a mark-sweep garbage collector for class objects:

1. **Trigger**: GC runs at safepoints when `m_gcPending` is set and heap size
   exceeds the threshold. Safepoints are function entry and loop back-edges.
2. **Mark phase**: Precise scan via `LocalDescriptor` — only slots with
   `typeKind == RTK_Class || RTK_Struct` are scanned. No conservative
   byte-scanning (decoupled from stack frame physical layout). Marking uses
   an iterative worklist (not recursive) to avoid stack overflow on deep
   object chains.
3. **Sweep phase**: Unmarked class objects are freed. Class-owned struct
   fields (value semantics) are freed with their owning class. Class fields
   (reference semantics) are freed independently by GC if unreachable.
4. **Free list**: Swept slots are added to a free list. New allocations
   prioritize reuse of free slots.

**Design decisions** (documented in VmExecutor.h):
- Safepoint-triggered, not allocation-point-triggered (avoids tracking
  tempSlot/tempSlot2 in MarkPhase)
- Precise scan via LocalDescriptor, not conservative byte scan (decouples
  GC from stack frame physical layout)
- `m_slotStructIdx` parallel array for struct type identification (structs
  have no type header; smaller change than adding one)

### Struct Lifetime

Struct objects are not individually garbage-collected. Their lifetime is tied
to their owner:
- Local struct variables: live until the function returns (or the variable is
  reassigned, at which point the old struct's owned nested structs are freed
  by deep-copy logic)
- Class-owned struct fields: freed when the owning class object is swept by GC
- Struct-owned nested structs: freed recursively with their parent

## Exit Code Convention

The process exit code is 8-bit (0-255) on Windows. Test expected values must
not exceed 255. For tests requiring larger computations, use modular arithmetic
or return a derived value that fits in the exit code range.

## Known Limitations

- **User-defined generics**: `class Foo<T> { ... }` is not supported. Only
  built-in generic classes (`List<T>`, `Dict<K,V>`) are recognized.
- **Bare `{...}` collection init**: dict/struct/class init requires the
  explicit `new Type{...}` form (the bare `{...}` form conflicts with
  block-statement grammar). See Collection Initializers above.
- **Bare `[]` empty init**: use `new List<T>{}`, `new Dict<K,V>{}`, or
  `new int[0]` instead. The lexer tokenizes `[]` as a single token used
  by the array-type suffix rule.
- **Nested generics (`List<List<int>>`, `Dict<K, List<V>>`)**: blocked
  by the lexer tokenizing `>>` as right-shift. Future phase may split
  `>>` in type context.
- **Bare init list as function argument**: requires `new Type{...}`
  explicit form. Phase 8e-6 overload uniqueness (Phase G) deferred.
- **`List<struct>` value semantics**: adding the same struct variable
  to a List twice shares the underlying heap slot (reference semantics
  at the boxing layer). Use separate struct instances for distinct
  elements.
- **`Dict<K,V>` with interface type**: interface types are not
  supported as generic type arguments. Use concrete class types.
- **Nested-subscript receiver write** (Phase 9d-3 leftover): in
  `matrix[i][0].x = v` (array-of-array-of-struct), the receiver's
  inner subscript index evaluation can clobber the RHS temp slot.
  Single-level `arr[i].field = v` works correctly. Fix deferred to the
  array redesign.
- **Eager materialization cost**: `new Point[n]` allocates n+1 heap
  slots at creation (array + one struct per element). Cost revisited at
  the array redesign.
- **Default parameters on imported functions**: cross-module imported
  functions support **constant-foldable** defaults only — int / float /
  string / null literals, plus single negation of numeric literals
  (`-5`, `-3.14`). Complex defaults (identifier references like
  `b = a`, function calls like `b = helper()`, casts, binary
  expressions other than unary `-`, `this.field` references) are
  rejected at the **consumer side** with a compile error. Producers
  (the imported module) accept any default expression; the restriction
  applies only when the consumer imports the function. Workaround for
  complex cross-module defaults: write a wrapper in the producer
  module that has only literal defaults, and have the consumer call
  the wrapper.
- **Named arguments on imported functions**: cross-module named
  arguments (`foo(b = 5, a = 3)` where `foo` is imported) are not
  supported. The consumer-side stub uses placeholder formal names
  (`p0`, `p1`, ...) because the `.nmod` format does not carry formal
  names. Use positional arguments only when calling imported
  functions.
- **Default parameters on interface methods**: interface method
  declarations (no body) do not have their defaults resolved. Callers
  must supply all arguments.
- **Method inheritance of defaults**: derived class overrides do not
  inherit default values from the base class method. Each override
  declares its own defaults independently.
- **Parameter count ceiling**: functions with more than 64 parameters
  (`kMaxFuncParams` sanity ceiling) trigger a compile-time error. The
  frame layout is otherwise dynamic — callParamBase and evalArea are
  sized per-function based on actual call patterns observed in the body.
- **No control flow in finally bodies**: `break` / `continue` / `return` /
  `throw` inside a `finally` body is a compile error (a finally body must
  not swallow the in-flight control flow or exception).
- **`super()` chains only to the direct parent**: there is no syntax for
  invoking a grandparent constructor directly; each ctor forwards to its
  immediate parent.
- **`new C(args)` when `C` has no constructor silently drops `args`**:
  unlike an explicit `super(args)` (which errors), constructor arguments
  at allocation sites are discarded when the class declares no ctor.
