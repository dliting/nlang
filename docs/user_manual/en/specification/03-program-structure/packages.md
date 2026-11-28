# 3.9 Packages and Import Search

## 3.9.1 File Roles

NLang moves code in four kinds of file.

- **`.n` — source file.** One file is one compilation unit (§3.1)
  and defines one module. The module path is the file's path relative
  to the search root (§3.9.2), one dotted segment per directory; no
  in-file syntax names a module.
- **`.ncu` — unit image.** The compiled artifact of one module.
  Cross-module references inside it are *import slots* — a target
  module path and a qualified name — never bodies (§3.9.3). The
  format is chapter 28.
- **`.npkg` — package archive.** A self-describing container of the
  build's members: a unit image for each module, named by its module
  path, plus the native host libraries the build ships, each named
  by its file name (§29.3). A project build produces a program
  package with an *entry record* naming the entry unit (§3.2); an
  importable library package omits the record. The standard library
  ships as thirteen domain packages, including `io.npkg` and
  `math.npkg` (§3.9.4). The format is chapter 29.
- **Native host library.** The host dynamic library that carries
  `native` implementations, named from the declaring module, which
  is necessarily single-segment (§7.5).

A module path comes from that one source — the path under a search
root — and nothing else:

```nlang
// stdlib/io.n              -> module "io"
// <root>/vendor/graphics.n (with -I <root>) -> module "vendor.graphics"
// <root>/gfx/color/deep.n  (with -I <root>) -> module "gfx.color.deep"
import io;
import vendor.graphics;
import gfx.color.deep;
```

A build whose units supply no entry (§3.2) produces a library
package: importable by other builds, not runnable.

*Informative:* a `native` declaration's host dynamic-link library
(DLL) is named from the module path's first segment
(`nlang_<segment>.dll`), so `native` inside a multi-segment module
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

An import of the module path `a.b.c` (§3.3) can be supplied by
source or by a compiled library. At compile time, resolution follows
these rules:

- Source has priority over artifacts across the whole search path. A
  source match is `a/b/c.n` under a search root. Exactly one match is
  inlined into the compilation (§3.1); each file is inlined once, and
  its imports are resolved by these same rules to a fixed point. Two
  or more source matches are a compile-time error naming the paths — a
  module path is unique in a build and is never silently shadowed.
- With no source match, artifact lookup visits the search directories
  in order. In one directory it checks `a.b.c.ncu` first, then the
  member tables of its library packages in file-name order. The first
  directory with a provider wins. The package file name does not take
  part in module resolution: a member named `a.b.c` supplies that
  module from any library package. This rule is identical for
  single-segment and dotted module paths.
- Two library packages in that winning directory which both contain
  the same module are a compile-time error, for example
  `Duplicate package 'a.b.c': '<first>.npkg' and '<second>.npkg'.`
  A program package — one with an entry record — is an executable
  closure, not a compile-time import surface, and is ignored here.
- A wildcard such as `import a.b.*;` takes the union of source modules
  and library-package members named `a.b` or beginning with `a.b.`.
  An empty union is a compile-time error. Every selected unit's own
  imports are then resolved to the same fixed point, so dependencies
  carried only by compiled images are included as well.

A matched artifact contributes declaration stubs. Source inlining also
serves declaration analysis; neither form copies a library function body
into the consumer's unit image. The compiled artifact records its
module-path dependencies and links to their bodies at run time (§3.9.3).
Source and artifact forms therefore expose the same import and qualified-
name behavior.

If no source or artifact supplies a module, the standard not-found
diagnostic names the module and the search-path remedy. Every package
that could not be opened while scanning is appended as a note:

```text
Module 'a.b.c' not found. Check the project Sources list or -I import path.
Note: package '<path>/broken.npkg' could not be opened.
```

A package whose member table names the module but whose member image is
invalid is instead reported as a failed module load; it is not described
as missing. Library-package member paths also participate in the
unimported-module diagnosis: if `tp.sub` exists only in an artifact,
using `tp.sub.Fn.f()` without an import still suggests
`import tp.sub;`, not the unusable `import tp.sub.Fn;`.

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
symbol with a package-name prefix — F12, F6 or Ctrl+click (a pointing-hand cursor
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

Module identity is the dotted module path. One build carries at most
one source or artifact unit for each module path (§3.9.2), and every
class, struct and function in a compiled artifact is keyed by its
fully qualified name (`main.main`, `alib.Point`);
implementation-owned built-ins keep their unqualified keys (`Object`,
`List`). The qualified key is the spelling of linkage — linking,
streams and tools alike.

At run time the loader starts from the entry artifact and walks
the import closure eagerly: every module path an imported unit
names is resolved and loaded before execution begins. Resolution
follows the search path (§3.9.2): the entry archive's own members
first — siblings of an entry ship together — then each search
directory in order, a `<path>.ncu` file before the members of
any `.npkg` in the same directory (archives scanned in file-name
order). The first match wins, and each module path is loaded at
most once.

All loaded units link into one runtime module by qualified name:
a cross-module call reaches the body of the resolved artifact. A
program package's entry record selects the entry unit and function.
Without that record, the loader starts from the first member's
serialized entry key; a package produced as a library has no entry
function and is therefore not runnable.

A closure problem — a missing module, an unreadable artifact —
is collected and reported in one shot, listing every directory
searched, rather than surfacing one failure at a time.

The import graph must be acyclic: a module may not import,
directly or through a chain, a module that imports it back. A
cycle — whether the build's own units close it, or an external
artifact already contains one — is a compile-time error naming
the modules along the cycle, in cycle order. The same rule keeps
every module's declarations reachable in one copy: a cycle could
only link by bootstrapping stale images of each other, with each
class on the ring existing twice.

Static state crosses module boundaries by the lazy rules of
§4.8.8, not by load order: loading links code but initializes
nothing, and the first use of a static member — from the
declaring module or through an import — triggers that class's
own initializer first. A module no one touches initializes
nothing.

### Qualified names

Everything a module declares can be reached through its qualified
name — `module.member` (and `module.sub.member` for a dotted module
path). A qualified name is an **absolute module path**: it names
exactly one module, and an import never re-bases it — after
`import my.*;` the call `foo.bar.x()` still addresses the top-level
`foo.bar` module, never `my.foo.bar`. Nested module paths therefore
cannot collide through qualified names, whatever wildcards are in
scope.

An explicit import opens both spellings: after `import lib;` the
qualified `lib.f()` and the unqualified `f()` are both admissible — the
import adds `lib`'s functions to this file's unqualified-name pool. A wildcard
import opens qualified calls only. Same-directory files share the
directory's unqualified-name pool without any import.

Each member's declared access applies on top of the gate.
`private` confines a member to its own unit; `protected` opens a
class member to the bodies of its subclasses, foreign subclasses
included (§4.8.2, §4.8.11), and adds nothing over `private` for a
top-level declaration while a package holds a single module (§7.1);
`public` — the default for top-level functions and types — admits
every gated unit. A denied member is invisible to lookup rather
than rejected after the fact: a foreign private type does not
resolve in a qualified name (`Type 'Vault' is not a member of
package 'lib'.`), and a foreign private function reports the
standard not-found diagnostic on either path. Within its own unit
every member is reachable regardless of its access.

Qualified type names are spelled the same way: `alib.Point`,
`gfx.color.deep.Shade`. Unlike functions, a type is also reachable by
its unqualified name program-wide, whatever module declares it — the
normative rule is §3.6; the declared access filters the foreign
candidates there the same way. Two module paths sharing a last
segment (`a.io`, `b.io`) identify two different modules; each keeps
its own functions and types.

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
module declares two functions of the same name and parameter count with
different parameter types, the call site's type-based selection travels to
the linker, which connects each call to the selected overload's own record.
An imported overload set therefore links exactly, with no name-and-arity
ambiguity. See [Overload Resolution](../07-functions/overloads.md#cross-module-overloads).

### Streams and literals

`readStruct`/`readObject` take the type name as a string. That literal
is resolved **at compile time** against the modules visible to the
file (its own module plus the imported ones) into the declaration's
qualified table key, so what reaches the VM is never an unqualified
name. Two visible modules declaring the same type name make the
literal ambiguous — the compile error asks you to qualify it
(`readStruct("alib.S")`). Object streams store the qualified class key
too: an object written by one program and read by another must use the
same module layout, because the reader looks the key up directly.

### Generics on qualified types

Type arguments are supported on the built-in generic containers
(`List<int>`, `Dict<string, int>`). A qualified **user** type with
type arguments (`alib.Box<int>`) is not part of the language: the
parser rejects the shape with a syntax error.

## 3.9.4 Standard Library Bootstrap

The standard library ships with the toolchain in two forms: `.n`
sources, discovered through the standard-library directory for
compile-time declaration analysis, and thirteen library packages,
loaded at run time from that directory. The packages contain fourteen
library units:

| Package archive | Module members |
| --- | --- |
| `concurrent.npkg` | `thread`, `concurrent` |
| `io.npkg` | `io` |
| `math.npkg` | `math` |
| `fs.npkg` | `fs` |
| `tensor.npkg` | `tensor` |
| `net.npkg` | `net` |
| `http.npkg` | `http` |
| `url.npkg` | `url` |
| `system.npkg` | `system` |
| `gzip.npkg` | `gzip` |
| `regex.npkg` | `regex` |
| `json.npkg` | `json` |
| `web.npkg` | `web` |

The standard-library directory is a system-default search entry
(§3.9.2), so an ordinary build finds both forms without configuration.
The package file names describe distribution domains; imports resolve
the module members listed in each package, not the archive file name.

One rule decides what is visible without an import: the language
core and the base containers are always in scope, while every other
standard-library module must be named by the importing unit (§3.3).
The always-visible names are declared by the preset modules `lang`
and `containers` — `Object`, the `Exception` family of §8.3,
`String` and `Array` in `lang`; `List<T>`, `Dict<K,V>` and
`Set<T>` in `containers` — and are the whole set. These two modules are embedded
at compile time and have no `.npkg` runtime members. A program that
uses any other standard-library name imports its module, including
the stream handle classes `ByteStream` and `FileStream` (§14.6).

The concurrency modules split by what they answer:

| Module | Provides |
| --- | --- |
| `thread` | who runs and where the result is fetched: `Thread`, `Future<T>`, `Promise<T>`, and the `thread.*` functions (§10.1, §10.5) |
| `concurrent` | how running computations coordinate: `Mutex`, `Semaphore`, `ReentrantMutex`, `AtomicInt`, `Channel<T>`, `ChannelClosedException`, `WaitGroup` (§10.2–§10.4, §10.6) |

Standard-library modules are imported like any other (§3.3);
chapters 13–27 describe them. The declarations in the `.n` sources are the
source of truth for every module, preset modules included; the
implementation binds their declared native members to built-in
implementations.

*Informative:* a third-party library may be distributed either as a
directory of `.n` source files or as one or more library `.npkg`
archives; native dynamic libraries are carried either loose
alongside or as archive members (§29.3). Both forms are addressed
by the same module paths. A function implemented
outside NLang is declared with the `native` keyword
(`native void print(string s);`) — such a declaration carries only
the signature and documentation, with no body; an ordinary function
without `native` is a readable, editable NLang implementation. A
library may contain both (a hybrid library, as in Python/Java/C#).
For the library-author side of the story, see
[Developing Libraries](../../libraries/developing-libraries.md).
