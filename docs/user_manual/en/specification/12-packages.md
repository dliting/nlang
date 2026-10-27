# 12. Packages and Import Search

## 12.1 File Roles

NLang moves code in four kinds of file.

- **`.n` — source file.** One file is one compilation unit (§3.1)
  and one *package*, the unit of code identity. A package's name
  is its path relative to the search root (§12.2) the file was
  found under, one dotted segment per directory; no in-file
  syntax names a package.
- **`.ncu` — unit image.** The compiled artifact of one package.
  Cross-package references inside it are *import slots* — a
  target package path and a qualified name — never bodies
  (§12.3). The format is chapter 20.
- **`.npkg` — package archive.** A container of unit images, one
  member per package, each member named by its package path. A
  project build produces one with an *entry record* naming the
  entry unit (§3.2); a library archive omits the record — the
  standard library ships as `stdlib.npkg` (§12.4). The format is
  chapter 21.
- **Native host library.** The host dynamic library that carries
  `native` implementations, named from the declaring package,
  which is necessarily single-segment (§7.5).

A build whose units supply no entry (§3.2) produces a library
artifact: importable by other builds, not runnable.

## 12.2 Import Resolution

One ordered set of directories — the *search path* — serves
compile-time source discovery, run-time artifact loading (§12.3)
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
  (§3.3, chapter 20).

Inlining resolves declarations for the importing compilation; it
copies no bodies. The compiled artifact names its dependencies
by package path and links against them at run time (§12.3).

## 12.3 Module Identity and Loading

Package identity is the dotted name. One build carries at most
one source for each package name (§12.2), and every class,
struct and function in a compiled artifact is keyed by its
package-qualified name (`main.main`, `alib.Point`);
implementation-owned built-ins keep their bare keys (`Object`,
`List`). The qualified key is the spelling of linkage — linking,
streams and tools alike.

At run time the loader starts from the entry artifact and walks
the import closure eagerly: every package path an imported unit
names is resolved and loaded before execution begins. Resolution
follows the search path (§12.2): the entry archive's own members
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

## 12.4 Standard Library Bootstrap

The standard library ships with the toolchain in two forms: `.n`
sources, discovered through the standard-library directory and
inlined at compile time, and `stdlib.npkg`, one member per
library unit, loaded at run time from the executable's
directory. Both directories are system-default search entries
(§12.2), so an ordinary build finds the standard library without
configuration.

Standard library packages are imported like any other (§3.3);
chapter 17 describes the units they hold. The classes the
implementation provides itself — the built-in value, container
and handle classes of chapters 4, 8 and 11 — need no import.

> Implementation note. The built-in classes are registered by
> the implementation today; the planned direction is to declare
> them in `.n` declaration files shipped as a builtins preset
> package, making those declarations the source of truth.
