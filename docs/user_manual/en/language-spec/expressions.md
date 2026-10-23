# Expressions


An expression produces a value. NLang is statically typed: every expression
has a type, and the compiler checks each operation against it.

### Expression pages

| Expression                                        | Page |
|---------------------------------------------------|------|
| operators (`+ - * / %`, comparison, logical)      | [Operators](operators.md) |
| casts (`as`, coercion to string)                  | [Type Casts](type-casts.md) |
| collection initializers (`[...]`, `Type{...}`) | [Collection Initializers](collection-initializers.md) |

String interpolation (`${...}`), escape sequences, and string comparison are
string-literal features: see [String](string.md). Char literals (`'a'`,
`'A'`) and type semantics are on [Primitives](primitives.md).

### Member access

```nlang
obj.field          // field read
obj.field = value  // field write
obj.method(args)   // method call
```

For class objects, `obj` must be non-null (runtime null check — see
[Class](class.md) "Null check"). A struct has no general methods — only
constructors (see [Struct](struct.md)); a field is read or written through
the member expression. Enum members are constants, not members of a value
(see [Enum](enum.md)).

### Creation and initialization quick reference

| Type | Creation forms | Details |
|---|---|---|
| class | constructor call `Node(42)`; direct-init declaration `Node n(42);`; braced anchor `P{value: 42}` | [Class](class.md) "Creating instances" |
| struct | constructor call `Point(1, 4)`; direct-init declaration `Point p(2, 3);`; named entries `Point{x: 1, y: 2}`; positional entries `Point{3, 7}`; bare declaration zero-initializes `Point p;` | [Struct](struct.md) "Creating and initializing" |
| enum | qualified member `Color.Red`; implicit int conversion `Color c = 1;` | [Enum](enum.md) |
| arrays | expression position `int[3]`, `[1, 2, 3]`; declaration position `int a[3];`, `int b[100] = [7, 8];` | [Array](array.md) |
| interface | upcast `IShape s = sq;` — an interface value always refers to an implementing object | [Interface](interface.md) |
| List/Dict | literals `[1, 2]`, `{"k": 1}`; empty `{}` | [Collection initializers](collection-initializers.md) |

A constructor call `Node(42)` is an expression: it allocates on the heap, runs the constructor with the arguments, and returns a reference (see [Class](class.md) "Constructors"). A struct constructor call `Point(1, 4)` is an expression too; braced entries `Point{...}` fill fields directly without running a constructor (see [Struct](struct.md) "Creating and initializing"). Declaration-position forms (`T name(args);`, `T name[n];`, `T name[n] = [...]`) are allowed on every declarator of a multi-declarator declaration (see [Declarations](declarations.md)).
