# ncc — Compile and Execute

ncc compiles source into a `.nmod` bytecode module and can run it
immediately. It is the everyday entry point: `ncc <file>` for a quick
look at a single source's result, the `build` form to produce a module
for nvm or ndb, and `-p` for multi-source projects. Build scripts and
automation pipelines use it too.

## Invocation forms

| Form | Command | Behavior |
|---|---|---|
| Compile and execute | `ncc <source.n> [-o out.nmod] [-I <dir>...]` | Compiles, then runs immediately |
| Compile only | `ncc build <source.n> [-o out.nmod] [-I <dir>...]` | Produces a .nmod |
| Project: compile and execute | `ncc -p <project.nproj> [-o out.nmod] [-I <dir>...]` | Compiles the project, then runs |
| Project: compile only | `ncc build -p <project.nproj> [-o out.nmod]` | Produces a .nmod |
| Execute only | `ncc run <module.nmod>` | Same as nvm; extra arguments are ignored |

## Flags

| Flag | Forms | Meaning |
|---|---|---|
| `-o <path>` | 1-4 | Output .nmod path; giving it twice is an error |
| `-p <nproj>` | project forms | Cannot be combined with a source positional; giving it twice is an error |
| `-I <dir>` (or `-I<dir>` glued) | 1-4 | .nmod import search path; may be given repeatedly |

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

## Default output locations

Without `-o`, the artifact location depends on the form:

- **Single-file forms** (`ncc <source.n>` / `ncc build <source.n>`):
  written to the **current working directory**, named after the source
  file stem — not next to the source. Think of this first when the
  artifact seems missing.
- **`-p` forms** (`ncc -p <nproj>` / `ncc build -p <nproj>`): written
  **next to the .nproj**, named after the project; the success line
  shows the artifact's full path in this case.

The .nproj's `outputDir` attribute redirects the artifact: a relative
path resolves against the project file, an absolute path replaces the
output directory outright.

## Multi-source projects (.nproj)

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Project name="hello_project" namespace="hello_project">
  <Sources>
    <File path="main.n"/>
    <File path="utils.n"/>
  </Sources>
</Project>
```

`name` is the output module name (defaults to the file stem),
`outputDir` optionally redirects the `.nmod` (relative to the project
file), and `File` paths are relative to the project file's directory.
See `examples/hello_project/` for a complete example.
