# Libraries and search paths

NLang libraries are carried by **`.n` source files**: the standard
`math.n`/`io.n`/`fs.n` ship with the toolchain, and a third-party library is
just a directory of `.n` files (optionally alongside native dynamic
libraries). A function implemented outside NLang is declared with the
`native` keyword (`native void print(string s);`) — such a declaration carries
only the signature and documentation, with no body; an ordinary function
without `native` is a readable, editable NLang implementation. A library may
contain both (a hybrid library, as in Python/Java/C#).
At compile time signatures are resolved by inlining the sources; at run
time unit images are loaded from the compiled library packages — the
standard library is the `stdlib.npkg` shipped with the toolchain (one
member per library unit).

The **search path** determines where the compiler looks for imported `.n`
files and where the run time loads the `.ncu`/`.npkg` members an
artifact depends on, plus the native dynamic libraries — the
standard library and third-party libraries, compile-time discovery and
run-time loading all use the **same set of directories**. Directories are
assembled in the following order, earlier ones winning; duplicates keep only
the first occurrence (paths are normalized, and case-folded on Windows):

1. command-line `-I <dir>` (highest priority; repeatable);
2. `<ImportPaths>` in the `.nproj` project file;
3. the project / source / module directory (local);
4. the `NLANG_PATH` environment variable (`;` on Windows, `:` on POSIX);
5. system defaults: the standard-library directory, the executable directory,
   the current directory (lowest).

Command-line usage:

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
