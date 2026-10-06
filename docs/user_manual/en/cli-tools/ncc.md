# ncc — Compile and Execute

ncc compiles source into a bytecode artifact and can run it immediately.
It is the everyday entry point: `ncc <file>` for a quick look at a single
source's result, the `build` form to produce a module for nvm or ndb, and
`-p` for multi-source projects. Build scripts and automation pipelines
use it too. Artifacts come in two forms: single-source builds produce a
`.ncu` unit image, project builds a `.npkg` program archive (see
"Artifacts and load-time linking").

## Invocation forms

| Form | Command | Behavior |
|---|---|---|
| Compile and execute | `ncc <source.n> [-o out.ncu] [-I <dir>...]` | Compiles, then runs immediately |
| Compile only | `ncc build <source.n> [-o out.ncu] [-I <dir>...]` | Produces a .ncu |
| Project: compile and execute | `ncc -p <project.nproj> [-o out.npkg] [-I <dir>...]` | Compiles the whole project, then runs |
| Project: compile only | `ncc build -p <project.nproj> [-o out.npkg]` | Produces a .npkg |
| Execute only | `ncc run <program.ncu\|.npkg>` | Same as nvm; extra arguments besides `-I` and `--verbose`/`-v` are an error |

## Flags

| Flag | Forms | Meaning |
|---|---|---|
| `-o <path>` | 1-4 | Output path — a `.ncu` for single-source forms, a `.npkg` for project forms; giving it twice is an error |
| `-p <nproj>` | project forms | Cannot be combined with a source positional; giving it twice is an error |
| `--no-warn` | 1-4 | Suppresses compile warnings (e.g. the implicit int→float precision-loss warning) |
| `-I <dir>` (or `-I<dir>` glued) | 1-5 | Library search path, may be given repeatedly: at compile time it locates imported `.n` files and external `.ncu`/`.npkg`; at run time it locates closure members and native dynamic libraries |
| `--verbose` / `-v` | 1-5 | Prints the resolved import search path — one directory per line, annotated with the layer it came from — then proceeds normally (the listing shape is the same as nvm's) |

Form 5 (`ncc run`) does not take `--no-warn`; like `-o` and `-p`, it is
rejected as an unexpected extra argument.
Errors and diagnostics go to stderr; the `Compiled successfully:` line
goes to stdout. Common error forms:

```text
Error: option -o given more than once.
Error: option -o requires a value.
Error: unexpected extra argument 'extra.n'.
Error: 'examples/hello_project/hello_project.nproj' looks like a project file; use -p examples/hello_project/hello_project.nproj
```

(When a .nproj is passed as a source positional, ncc suggests the `-p`
form directly.)

## Artifacts and load-time linking

NLang artifacts **do not embed library code**: every participating source
file yields its own **unit image**, and cross-package symbols
(`io.print`, `utils.helper.answer`) are recorded in the image as **import
slots**. At load time the loader discovers every dependency along the
library search path — library archives such as `stdlib.npkg`, external
`.ncu` files — and links them with the artifact into one runtime module.
nvm, `ncc run`, and ncc's compile-and-execute all go through this same
load-and-link path and behave identically.

- **`.ncu` (unit image)** — the artifact of a single-source build,
  carrying the entry unit. To run it, any external module it imports
  must still be locatable on the search path (the standard library ships
  with the toolchain and is found automatically; third-party libraries
  need `-I` or `NLANG_PATH`).
- **`.npkg` (package archive)** — the artifact of a project build: one
  member per unit in the project (the member name is the module path),
  plus an entry record. The same format serves pure library
  distribution (`stdlib.npkg` is a library package whose members are
  `io`/`math`/`fs`).

A missing dependency is reported in one shot, listing every directory
searched:

```text
Runtime error: nloader failed:
  module 'lib' not found (searched: <dir>, ...)
```

Check first whether `-I` and `NLANG_PATH` cover the directory holding
the dependency.

## Default output locations

Without `-o`, the artifact location depends on the form:

- **Single-file forms** (`ncc <source.n>` / `ncc build <source.n>`):
  written to the **current working directory**, named after the source
  file stem — not next to the source. Think of this first when the
  artifact seems missing.
- **`-p` forms** (`ncc -p <nproj>` / `ncc build -p <nproj>`): written
  **next to the .nproj**, the archive named after the project (the
  artifact is a `.npkg`); the success line shows the artifact's full
  path in this case.

The .nproj's `outputDir` attribute redirects the artifact: a relative
path resolves against the project file, an absolute path replaces the
output directory outright.

## Multi-source projects (.nproj)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` is the output module name (defaults to the file stem),
`outputDir` optionally redirects the `.npkg` (relative to the project
file), and `File` paths are relative to the project file's directory.
See `examples/hello_project/` for a complete example.

## Library search paths

Imported `.n` files are looked up across an ordered set of directories:
those given with `-I` first, then the `.nproj`'s `<ImportPaths>`, the
source/project directory, the `NLANG_PATH` environment variable (`;` on
Windows, `:` on POSIX), and finally system defaults such as the
standard-library directory. Earlier directories win; duplicates keep only
the first occurrence. The same directory set keeps serving the run time:
closure loading (`.ncu`/`.npkg` member lookup) and native dynamic
library loading. `--verbose` (short form `-v`) makes ncc print this
resolved ordering (each line annotated with its layer, e.g. `(-I)`,
`(project import paths)`, `(local directory)`, `(NLANG_PATH)`,
`(system)`) before compiling.

A `.nproj` can persist search dirs under `<ImportPaths>` (paths stored
relative to the project file):

```xml
<Project name="app">
  <Sources><File path="main.n"/></Sources>
  <ImportPaths><Dir path="../libs"/></ImportPaths>
</Project>
```

For the full five-level order, `native` libraries and graphical
configuration in nide, see
["Libraries and search paths"](../language-spec/stdlib-search-paths.md).
