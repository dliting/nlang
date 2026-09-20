# Command-line Tools

Five tools ship in the installation's `bin\` directory, four of them
command-line tools: `ncc` (compile), `nvm` (run), `ndb` (debug), and
`ndisasm` (disassemble); the fifth is the nide graphical IDE — see
[Three Ways to Run](running.md). This page is the complete reference for
the four command-line tools. If PATH is not set up, prefix commands with
`bin\` (e.g. `bin\ncc build ...`).

<!-- This page and the Command-line Tools section of README.md are two outlets of the same facts: when the command surface changes, both must be updated; depth may differ. -->

## ncc — Compile and Execute

### Invocation forms

| Form | Command | Behavior |
|---|---|---|
| Compile and execute | `ncc <source.n> [-o out.nmod] [-I <dir>...]` | Compiles, then runs immediately |
| Compile only | `ncc build <source.n> [-o out.nmod] [-I <dir>...]` | Produces a .nmod |
| Project: compile and execute | `ncc -p <project.nproj> [-o out.nmod] [-I <dir>...]` | Compiles the project, then runs |
| Project: compile only | `ncc build -p <project.nproj> [-o out.nmod]` | Produces a .nmod |
| Execute only | `ncc run <module.nmod>` | Same as nvm; extra arguments are ignored |

### Flags

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

### Default output locations

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

### Multi-source projects (.nproj)

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

## nvm — Run a Module

```text
nvm <module.nmod> [--gc-stress=N]
```

Runs a compiled module; the process exit code is `main`'s return value
(conventions on the [exit code page](../language-spec/exit-code-convention.md)).
A module that cannot be opened reports
`Runtime error: Failed to open module file: <path>`. `--gc-stress=N` is
a testing knob: it clamps both GC thresholds to tiny values so any
untraced reference goes stale within a few allocations — useful for
verifying memory-management changes, not needed in everyday use.

## ndb — Debugger

```text
ndb <module.nmod>
```

After loading the module it **stops at the first statement of the
entry point** (like gdb's `start`), prints the `(ndb) ` prompt, and
reads commands from stdin one per line; stdin EOF behaves like `q`.

### Command table

| Command | Long alias | Meaning |
|---|---|---|
| `b <file.n:LINE \| LINE \| funcName>` | break | Set a breakpoint; a bare `LINE` resolves in the current frame's file; all functions of the same name are hit |
| `i b` | info | Breakpoint list (with hit counts) |
| `d <id>` | delete | Delete a breakpoint |
| `c` | continue | Continue |
| `s` | step | Step into |
| `n` | next | Step over |
| `f` | finish | Step out of the current function |
| `bt` | backtrace | Call stack |
| `frame <n>` | — | Select a frame |
| `info locals` | info | Locals of the selected frame (hidden names filtered) |
| `p <name>` | print | Print one local |
| `l [line]` | list | Source window (current line marked `->`) |
| `x` | — | Disassembly of the selected frame (current instruction marked `>>`) |
| `catch on\|off` | — | Break on throw (default off) |
| `help` | — | Command help |
| `q` | quit | Quit (kills the debugged program) |

The short forms are the canonical command surface (matching the `help`
output); long aliases are accepted as equivalents.

A complete session (debugging `examples/hello_project`, breakpoint set
by function name in the second file):

```console
$ ncc build -p examples/hello_project/hello_project.nproj -o hello_project.nmod
Compiled successfully: hello_project.nmod
$ ndb hello_project.nmod
Stopped: main (main.n:6)
(ndb) b addBoth
Breakpoint 1 at addBoth (utils.n:2)
(ndb) c
Breakpoint 1, addBoth (utils.n:2)
(ndb) bt
#0  addBoth (utils.n:2)
#1  main (main.n:6)
(ndb) info locals
a = 40
b = 2
(ndb) p a
a = 40
(ndb) c
Program exited with code 0.
```

When the program finishes, ndb prints `Program exited with code N.` and
exits with that same code; `q` or stdin EOF kills the program and exits
ndb itself with 0.

### Protocol for embedding front ends

`ndb --machine <module.nmod>` exposes the same session over a
tab-separated line protocol on stdin/stdout for embedding front ends —
the nide graphical debugger is built on it. Protocol details in
[Debugging in nide](debugging.md) and
[Debugger Architecture](../vm-architecture/debugging.md).

## ndisasm — Disassembler

```text
ndisasm <module.nmod>
ndisasm -func <name> <module.nmod>
```

Two invocations: a full dump, or `-func <name>` keeping only one
function section (the other sections remain). **`-func` must precede
the module path** — after it, the flag is silently ignored and the
output equals the full dump. The output sections come in a fixed order:
`module:` → `structs:` → `classes:` → `string constants:` → one
section per function.

The function header line carries all metadata (`file=` mirrors the
source path form passed at compile time; built-in functions carry
`intrinsic=N` and no `file=`):

```text
function main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
  bytecode:
    0000: debug 2
    0003: const_i32 42
    0008: assign 0
    ...
```

A full dump lists every built-in class method (all `(no bytecode)` —
seventy-plus even for a hello), so day-to-day inspection of one
function uses the `-func` filter. Typical uses: cross-checking ndb's
`x` command against instruction addresses, reviewing optimization
results, and diagnosing serialization problems.

## Version Queries

All four tools report their version with `--version`, in the form
`ncc (NLang) <version>`; nide shows its version in Help → About, and
the documentation site in its footer.
