# Declarations


Type declarations — `enum`, `struct`, `class`, `interface` — each live on their
own page: [Enum](enum.md), [Struct](struct.md), [Class](class.md),
[Interface](interface.md). This page covers the remaining declarations:
imports, variables, and type aliases.

### Import Declaration

```nlang
import io;                 // built-in namespace
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
| Built-in namespace | `io` / `math` / `fs` (reserved names, preset modules) | `io` |

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
- Known limitation: members of a namespace shared across directories
  (two files declare the same `namespace NS`) are currently unreachable
  from another directory — bare-name resolution covers only the own file
  plus same-directory files, and no qualified form exists because a
  module path addresses root-level functions only, so the
  `import it and qualify the call` hint's suggested fix does not work
  for them.
- Resolution order for an import target: built-in → project file →
  external `.nmod` (via `-I`). No implicit fallback.
- Project path segments may not collide with `io`/`math`/`fs` (compile
  error). Single-file mode (no `.nproj`) supports single-segment imports
  only — built-ins and external `.nmod`; dotted paths cannot resolve.

Diagnostics (examples):

```
Module 'utils.helper' is not imported. Add 'import utils.helper;' (or 'import utils.*;') at the top of this file.
Namespace 'io' is not imported. Add 'import io;' at the top of this file.
Module 'utils.helper' not found. Check the project Sources list or -I import path.
String import is removed. Use 'import <module>;' with an identifier path.
Module path segment 'io' collides with a built-in namespace.
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
default to null. The type-specific declaration forms and their semantics
are on each type's page: [Enum](enum.md), [Struct](struct.md),
[Class](class.md), [Interface](interface.md).

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
  or the reserved stdlib namespaces (`math`, `io`, `fs`) in the same
  translation unit.
- The namespace-opening form `using Foo;` (no `=`) is unchanged and
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
