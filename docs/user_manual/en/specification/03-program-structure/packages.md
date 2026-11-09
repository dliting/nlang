# 3.9 Packages and Import Search

## 3.9.1 File Roles

NLang moves code in four kinds of file.

- **`.n` — source file.** One file is one compilation unit (§3.1)
  and one *package*, the unit of code identity. A package's name
  is its path relative to the search root (§3.9.2) the file was
  found under, one dotted segment per directory; no in-file
  syntax names a package.
- **`.ncu` — unit image.** The compiled artifact of one package.
  Cross-package references inside it are *import slots* — a
  target package path and a qualified name — never bodies
  (§3.9.3). The format is chapter 25.
- **`.npkg` — package archive.** A container of unit images, one
  member per package, each member named by its package path. A
  project build produces one with an *entry record* naming the
  entry unit (§3.2); a library archive omits the record — the
  standard library ships as `stdlib.npkg` (§3.9.4). The format is
  chapter 26.
- **Native host library.** The host dynamic library that carries
  `native` implementations, named from the declaring package,
  which is necessarily single-segment (§7.5).

A package's name comes from that one source — the path under a
search root — and nothing else:

```nlang
// stdlib/io.n            -> package "io"
// <root>/vendor/graphics.n (with -I <root>) -> package "vendor.graphics"
// <root>/gfx/color/deep.n  (with -I <root>) -> package "gfx.color.deep"
import io;
import vendor.graphics;
import gfx.color.deep;
```

A build whose units supply no entry (§3.2) produces a library
artifact: importable by other builds, not runnable.

*Informative:* a `native` declaration's host dynamic-link library
(DLL) is named from the package's first segment
(`nlang_<segment>.dll`), so `native` inside a multi-segment package
(`gfx.color.deep`) cannot name a host library and is a
compile-time diagnostic.

## 3.9.2 Import Resolution

One ordered set of directories — the *search path* — serves
compile-time source discovery, run-time artifact loading (§3.9.3)
and native host libraries (§7.5). The path is assembled in this
order, earlier entries first:

1. each `-I <dir>` command-line option, in order of appearance;
2. the `<ImportPaths>` entries of the project file;
3. the local directory: the project, source or module directory;
4. the `NLANG_PATH` environment variable, split on `;` on
   Windows and `:` on POSIX;
5. the system defaults: the standard-library directory, the
   executable's directory, the current directory.

Entries are normalized; a directory listed more than once keeps
its first occurrence (paths compare case-insensitively on
Windows). Every tool includes all three system-default
directories; their relative order is *unspecified* (§0.4).

An import of the package path `a.b.c` (§3.3) names the source
file `a/b/c.n` under a search root. At compile time:

- if exactly one root offers the file, that unit is inlined into
  the compilation (§3.1); each file is inlined once, and the
  imports of an inlined unit are resolved by the same rules, to
  a fixed point;
- if two or more roots offer it, it is a compile-time error
  naming the source paths: a package name is unique in a build,
  never silently shadowed;
- if no root offers a source: a single-segment path `c` (§3.3)
  resolves to an external artifact — the first search directory
  holding `c.ncu` or `c.npkg`, a unit image before an archive of
  the same name — whose declarations enter the build as stubs;
  a dotted path with no source in the build is a compile-time
  error, since external artifacts carry single-segment names
  (§3.3, §25.2).

Inlining resolves declarations for the importing compilation; it
copies no bodies. The compiled artifact names its dependencies
by package path and links against them at run time (§3.9.3).

*Informative:* how the search path is configured and used in
practice. Command-line usage:

```text
ncc build app.n -o app.ncu -I C:\libs\mylib
nvm app.ncu -I C:\libs\mylib
ndb --machine app.ncu -I C:\libs\mylib
```

A project persists its search dirs in `.nproj` under `<ImportPaths>` (paths
are stored relative to the project file):

```xml
<Project name="app">
  <Sources><File path="src/main.n"/></Sources>
  <ImportPaths><Dir path="../libs"/></ImportPaths>
</Project>
```

**Configuring in nide**: global search paths live under Tools → Options →
Library search paths, and project-level paths under Project → Properties →
Library search paths; both support add, remove, move up/down and browse,
with project paths taking precedence over global ones. A change rebuilds the
symbol index, refreshing completion and go-to-definition.

**Viewing and jumping to source**: use Go to Definition on a
package-qualified symbol — F12, F6 or Ctrl+click (a pointing-hand cursor
marks the jump targets while Ctrl is held) — to open its `.n` at the
declaration: a `native` declaration shows the signature and
documentation, an ordinary function shows an editable implementation. An
`import` statement's package name is itself a jump target: Go to
Definition on it opens the file that declares the package. While Ctrl is
held, a jumpable name renders as a blue underlined hyperlink, and with the
cursor on a jump target the Edit menu and the editor's context menu enable
their Go to Definition (F12) entries too.
The packages visible to jumps and completion cover the standard library,
the configured library directories, the open projects' directories and
each open file's directory; rebuild after editing to pick up the change.

## 3.9.3 Module Identity and Loading

Package identity is the dotted name. One build carries at most
one source for each package name (§3.9.2), and every class,
struct and function in a compiled artifact is keyed by its
package-qualified name (`main.main`, `alib.Point`);
implementation-owned built-ins keep their bare keys (`Object`,
`List`). The qualified key is the spelling of linkage — linking,
streams and tools alike.

At run time the loader starts from the entry artifact and walks
the import closure eagerly: every package path an imported unit
names is resolved and loaded before execution begins. Resolution
follows the search path (§3.9.2): the entry archive's own members
first — siblings of an entry ship together — then each search
directory in order, a `<path>.ncu` file before the members of
any `.npkg` in the same directory (archives scanned in file-name
order). The first match wins, and each package path is loaded at
most once.

All loaded units link into one runtime module by qualified name:
a cross-package call reaches the body of the resolved artifact.
An archive imported as a single module must expose exactly one
unit — an entry record, or exactly one member.

A closure problem — a missing package, an unreadable artifact —
is collected and reported in one shot, listing every directory
searched, rather than surfacing one failure at a time.

The import graph must be acyclic: a package may not import,
directly or through a chain, a package that imports it back. A
cycle — whether the build's own units close it, or an external
artifact already contains one — is a compile-time error naming
the packages along the cycle, in cycle order. The same rule
keeps every package's declarations reachable in one copy: a
cycle could only link by bootstrapping stale images of each
other, with each class on the ring existing twice.

Static state crosses module boundaries by the lazy rules of
§4.8.8, not by load order: loading links code but initializes
nothing, and the first use of a static member — from the
declaring module or through an import — triggers that class's
own initializer first. A module no one touches initializes
nothing.

### Qualified names

Everything a package declares can be reached through its qualified
name — `package.member` (and `package.sub.member` for a dotted
package). A qualified name is an **absolute module path**: it names
exactly one package, and an import never re-bases it — after
`import my.*;` the call `foo.bar.x()` still addresses the top-level
`foo.bar` package, never `my.foo.bar`. Nested packages therefore
cannot collide through qualified names, whatever wildcards are in
scope.

An explicit import opens both spellings: after `import lib;` the
qualified `lib.f()` and the bare `f()` are both admissible — the
import adds `lib`'s functions to this file's bare pool. A wildcard
import opens qualified calls only. Same-directory files share the
directory's bare pool without any import.

Qualified type names are spelled the same way: `alib.Point`,
`gfx.color.deep.Shade`. Unlike functions, a type is also reachable by
its bare name program-wide, whatever package declares it — the
normative rule is §3.6.
Two packages sharing a last path segment (`a.io`, `b.io`) are two
different packages; each keeps its own functions and its own types.

### Qualified table keys

*Informative:* the qualified key is also the debugger and tool spelling
wherever a symbol is named:

- breakpoints take the qualified name: `b main.main`,
  `b mathutil.triple`;
- backtraces and stop lines print it: `#0  main.main (main.n:6)`,
  `Stopped: utils.helper.inner (helper.n:6)`;
- `ndisasm -func main.main` filters by the same spelling;
- a value of function type renders with its key: `func alib.twice`.

*Informative:* where a value render names a type for the reader — an
object's default `toString()` (`Point@1a2b`) and the debugger's value
display (`Point{x=2, y=5}`) — the **leaf** is used: the last segment of
the qualified key, the name as source wrote it.

The qualified key remains the spelling of linkage (linking, streams,
disassembly); a value render shows the type, not its linkage identity.

Overloads resolve by parameter type across the boundary as well: when a
package declares two functions of the same name and parameter count with
different parameter types, the call site's type-based selection travels to
the linker, which connects each call to the selected overload's own record.
An imported overload set therefore links exactly, with no name-and-arity
ambiguity. See [Overload Resolution](../07-functions/overloads.md#cross-module-overloads).

### Streams and literals

`readStruct`/`readObject` take the type name as a string. That literal
is resolved **at compile time** against the packages visible to the
file (its own package plus the imported ones) into the declaration's
qualified table key, so what reaches the VM is never a bare name. Two visible packages declaring the same type name make the
literal ambiguous — the compile error asks you to qualify it
(`readStruct("alib.S")`). Object streams store the qualified class key
too: an object written by one program and read by another must come
from the same package layout, because the reader looks the key up
directly.

### Generics on qualified types

Type arguments are supported on the built-in generic containers
(`List<int>`, `Dict<string, int>`). A qualified **user** type with
type arguments (`alib.Box<int>`) is not part of the language: the
parser rejects the shape with a syntax error.

## 3.9.4 Standard Library Bootstrap

The standard library ships with the toolchain in two forms: `.n`
sources, discovered through the standard-library directory and
inlined at compile time, and `stdlib.npkg`, one member per
library unit, loaded at run time from the executable's
directory. Both directories are system-default search entries
(§3.9.2), so an ordinary build finds the standard library without
configuration.

One rule decides what is visible without an import: the language
core and the base containers are always in scope, everything
else is a package the importing unit names (§3.3). The
always-visible names are declared by the preset packages `lang`
and `containers` — `Object`, the `Exception` family of §8.3,
`String` and `Array` in `lang`; `List<T>` and `Dict<K,V>` in
`containers` — and are the whole set; a program
that uses any other standard-library name imports its package,
the stream handle classes `ByteStream` and `FileStream` (§14.6)
included.

The concurrency names split by what they answer:

| Package | Provides |
| --- | --- |
| `thread` | who runs and where the result is fetched: `Thread`, `Future<T>`, `Promise<T>`, and the `thread.*` functions (§10.1, §10.5) |
| `concurrent` | how running computations coordinate: `Mutex`, `Semaphore`, `ReentrantMutex`, `AtomicInt`, `Channel<T>`, `ChannelClosedException`, `WaitGroup` (§10.2–§10.4, §10.6) |

Standard library packages are imported like any other (§3.3);
chapters 13–21 describe the units they hold. The declarations in
the `.n` sources are the source of truth for every package,
prelude included; the implementation binds the declared native
members to its built-in implementations.

*Informative:* NLang libraries are carried by `.n` source files:
the standard library ships one `.n` for every package — the prelude
`lang.n` and `containers.n` plus the eleven standard packages — and a
third-party library is just a directory of `.n` files (optionally
alongside native dynamic libraries). A function implemented outside NLang is declared with
the `native` keyword (`native void print(string s);`) — such a
declaration carries only the signature and documentation, with no
body; an ordinary function without `native` is a readable,
editable NLang implementation. A library may contain both (a
hybrid library, as in Python/Java/C#). For the library-author
side of the story, see
[Developing Libraries](../../libraries/developing-libraries.md).
