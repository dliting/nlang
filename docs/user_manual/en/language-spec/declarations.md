# Declarations


Type declarations — `enum`, `struct`, `class`, `interface` — each live on their
own page: [Enum](enum.md), [Struct](struct.md), [Class](class.md),
[Interface](interface.md). This page covers the remaining declarations:
imports, variables, and type aliases.

### Import Declaration

```nlang
import io;                 // standard library package
import lib;                // external module lib.ncu / lib.npkg
import utils.helper;       // project file utils/helper.n
import utils.*;            // recursive wildcard
```

An `import` declares which modules this **file** may reference — the
import set belongs to the translation unit and never leaks to other
files. Three sources share one syntax:

| Source | Module path | Example |
|---|---|---|
| Project file | dotted path relative to the `.nproj` root: directory path + file stem | `utils/helper.n` → `utils.helper`; root `main.n` → `main` |
| External module | file stem (single segment): a `.ncu` file, or a `.npkg` exposing exactly one module | `lib.ncu` / `lib.npkg` → `lib` |
| Standard library package | `io` / `math` / `fs` / `thread` / `concurrent` (library sources shipped with the toolchain) | `io` |

Visibility:

| Reference | Import needed? | Call form |
|---|---|---|
| Same file | no | bare |
| Same directory, other project files | no (implicit) | bare **or** qualified |
| Cross-directory, same project | **yes** (`import utils.helper;` or `import utils.*;`) | qualified only: `utils.helper.f()` |
| External module | **yes** (`import lib;`) | qualified only: `lib.f()` |
| Standard library `io`/`math`/`fs`/`thread`/`concurrent` | **yes** (`import io;`) | qualified: `io.print` |

- Bare-name resolution covers only the own file plus same-directory
  files; everything else must be qualified by module path. Ownerless
  symbols (root built-ins such as the `Exception` class family, native
  host bindings) stay globally bare-visible.
- Wildcard `import utils.*;` is a **recursive prefix match** in module
  path space: every path starting with `utils.` is importable
  (`utils.helper`, `utils.sub.x`, ...). It only abbreviates the import
  list — calls still write the full path. Wildcards match project files
  only; external `.ncu` names are single-segment and never match.
- `import utils;` matches only the root file `utils.n`; to reach the
  `utils/` directory use the full path or a wildcard.
- Duplicate imports are idempotent; exact + wildcard overlap takes the
  union; importing the own module path or a same-directory file is a
  harmless redundancy.
- Resolution order for an import target: modules compiled into the build
  (project files or library sources on a search root — the standard
  library uses the same mechanism as a third-party source library) →
  external modules (`.ncu`/`.npkg`, via `-I`). No implicit fallback.
- Two units resolving to the same dotted package in one build are a
  compile error naming both source paths. A project directory named
  `io`/`math`/`fs` is an ordinary directory; only one package of each
  name may exist. Dotted imports resolve library sources under the
  matched search root (`-I <root>` + `<root>/a/b/c.n` addresses
  `import a.b.c;`); precompiled dotted packages are currently searched
  by their single last segment.
- An external module's code does not enter the artifact: the consumer
  image only records import slots, and the loader relocates the module
  at run time along the same search path (the mechanism is in
  [Command-line Tools / ncc](../cli-tools/ncc.md), "Artifacts and
  load-time linking").

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
Point pt;                   // struct (field defaults, or zero)
Node n = Node(7);        // class (heap-allocated)
Node n2;                    // class (null)
```

Struct variables take their field defaults — zero when no initializer, the
declared default when one is. Class variables default to null.

`T name();` is a **zero-argument construction**: it runs the type's
zero-argument constructor, at top level and inside function bodies alike
(`Foo f();` is the same as `Foo f = Foo();`). A primitive type in this
form is a compile error (`"Int32" is not a class or struct type.`). The
bodyless function-declaration spelling keeps the `func` marker
(`func Foo f();`).

A declaration may list several declarators separated by commas, and
every declarator may use any of the first declarator's forms — plain,
`= value`, `= {…}` initializer, `(args)` direct-init, `()` zero-argument
construction, `[n]` sized, `[n] = [...]` sized with entries:

```nlang
int a, b = 5;               // a is 0, b is 5
int xs[3], ys[2];           // two arrays of different lengths
Vec v(2), w();              // both constructed (w via the zero-argument constructor)
List<int> l, m = {1, 2};    // m gets the brace initializer
int p[3], q;                // p is an int array, q is a plain int
```

Each declarator's type is the head type plus its own brackets: in
`int a[3], b;` the `b` is a plain `int`, not an array. A head type that
is itself an array (`int[] a, b;`) makes every declarator an array.

A name introduced by any declaration — variable, parameter, field, or
loop variable — must not reuse a visible type name: with `class Counter`
in scope, `int Counter = 5;` reports
`"Counter" names a type; it cannot be used as a variable name.`

### Default Values

A declaration without an initializer gets the type's default value (no
compiler warning is issued):

| Type | Default |
|---|---|
| the 8 integer types (`byte`, `ubyte`, `short`, `ushort`, `int`, `uint`, `long`, `ulong`) | 0 |
| `float`, `double` | 0 |
| `bool` | false |
| `char` | code point 0 |
| `string` | null handle that reads as the empty string |
| enum | underlying value 0 |
| struct | every field's default (zero when no initializer) |
| class, interface, `Object` | null |
| arrays, `List`, `Dict` | null |
| `func` | null |

Reference types default to null: `== null` tests it directly, and using
the value without a null check — calling a method, accessing a member,
indexing, taking the length — raises a `null reference error` with a
backtrace at run time. `string` is the one exception: a null handle
reads as the empty string, so `.length()` is 0 and concatenation treats
it as `""` (see [String](string.md)). An enum's default is the underlying
value 0: with auto-incremented members that is exactly the first member —
after `Color c;`, `c == Color.Red` holds; an enum with explicitly
assigned non-zero values may default to 0, which lands on no member.

The type-specific declaration forms and their semantics
are on each type's page: [Enum](enum.md), [Struct](struct.md),
[Class](class.md), [Interface](interface.md).

### Type Aliases

`using Name = Type;` declares a **type alias** — a shorthand for any
type expression, usable everywhere a type is expected:

```nlang
using Grid = Dict<string, List<int[]>>;
using Ints = int[];
using BinOp = func<int, int>;

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
  aliases, built-in type names (`List`, `Dict`, `func`, `int`, ...),
  or an indexed library package name (`math`, `io`, `fs`, `thread`,
  `concurrent`, or a third-party package found on the search path) in
  the same translation unit.
- The scope-opening form `using Foo;` (no `=`) is unchanged and
  unrelated.

**Restrictions:**
- The right-hand side must be a **primitive type name** (`int`, `float`,
  `string`, etc.), an **array type** (`int[]`), a **generic instantiation**
  (`List<int>`), or a **function type** (`func<int, int>`). A bare
  **class/struct/enum type name** (`using X = Counter;`) is not a valid
  target — it fails as a parser syntax error. **Member paths**
  (`using X = ns.Inner;`) are also not supported — the declaration
  itself parses, but every use site fails to resolve.
- Aliases are type-position only; a value expression can never resolve
  to an alias.
- Diagnostics anchored at a use site sometimes point at the `using` line
  (the expansion clone prefers the alias target's location).
