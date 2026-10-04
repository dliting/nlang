# Packages

A **package** is NLang's unit of code identity. Every `.n` file is one
package, and the package name comes from exactly one source: the file's
path relative to the search root it was found under. There is no
in-file syntax that names a package — the wrapper keyword that older
versions carried was removed.

```nlang
// stdlib/io.n            -> package "io"
// <root>/vendor/graphics.n (with -I <root>) -> package "vendor.graphics"
// <root>/gfx/color/deep.n  (with -I <root>) -> package "gfx.color.deep"
import io;
import vendor.graphics;
import gfx.color.deep;
```

## Qualified names

Everything a package declares is reached through its qualified name —
`package.member` (and `package.sub.member` for a dotted package). The
import opens the qualified name only: after `import lib;` you write
`lib.f()`, and the bare `f()` does not resolve through the import. (A
file in the same directory as the caller shares that directory's bare
pool — the same-directory rule is unchanged.)

Types are qualified the same way: `alib.Point`, `gfx.color.deep.Shade`.
Two packages sharing a last path segment (`a.io`, `b.io`) are two
different packages; each keeps its own functions and its own types, and
one build may contain only one package of each dotted name — two
sources resolving to one package name are a compile error naming both
source paths.

## Compiled artifacts and qualified table keys

Every package compiles into its own **unit image** (`.ncu`, format
v2.0), and each struct/class/function table key in the image is
**package-qualified**: `main.main`, `utils.helper.help`, `alib.Point`.
Ownerless built-ins keep their bare keys (`Object`, `List`). References
into other packages are recorded as **import slots** (target module
path + qualified name); at run time the loader gathers the whole
import closure along the search path and links it by qualified name
into the one runtime module — a cross-package symbol has no body in
the artifact, only a name. The qualified key is also the debugger and
tool spelling wherever a symbol is named:

- breakpoints take the qualified name: `b main.main`,
  `b mathutil.triple`;
- backtraces and stop lines print it: `#0 main.main (main.n:6)`,
  `Stopped: utils.helper.inner`;
- `ndisasm -func main.main` filters by the same spelling;
- a value of function type renders with its key: `func alib.twice`.

Value renders that name a type for the reader — an object's default
`toString()` (`Point@1a2b`) and the debugger's value display
(`Point{x=2, y=5}`) — use the **leaf**: the last segment of the
qualified key, the name as source wrote it. The qualified key remains
the spelling of linkage (linking, streams, disassembly); a value
render shows the type, not its linkage identity.

## Streams and literals

`readStruct`/`readObject` take the type name as a string. That literal
is resolved **at compile time** against the packages visible to the
file (its own package plus the imported ones) and rewritten to the
declaration's qualified table key, so what reaches the VM is never a
bare name. Two visible packages declaring the same type name make the
literal ambiguous — the compile error asks you to qualify it
(`readStruct("alib.S")`). Object streams store the qualified class key
too: an object written by one program and read by another must come
from the same package layout, because the reader looks the key up
directly.

## Native declarations

A `native` declaration's host DLL is named from the package's **first
segment** (`nlang_<segment>.dll`), so `native` inside a multi-segment
package (`gfx.color.deep`) cannot name a host library and is a
compile-time diagnostic. Native declarations in single-segment
packages work as before.

## Generics on qualified types

Type arguments are supported on the built-in generic containers
(`List<int>`, `Dict<string, int>`). A qualified **user** type with
type arguments (`alib.Box<int>`) is not part of the language yet: the
parser rejects the shape with a syntax error. (A dedicated diagnostic
naming the limitation is planned for a later phase.)
