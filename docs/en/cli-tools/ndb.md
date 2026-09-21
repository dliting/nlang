# ndb — Debugger

ndb is NLang's interactive debugger: it loads a module, stops at the
entry point, and follows commands for breakpoints, stepping, locals,
and the call stack. Reach for it when a program misbehaves; debugging
cross-file projects (breakpoints by `file:line` or function name) is
the primary use case. The same session is also exposed to embedding
front ends as a line protocol (`--machine`) — nide's graphical
debugger is built on it.

```text
ndb <module.nmod>
```

After loading the module it **stops at the first statement of the
entry point** (like gdb's `start`), prints the `(ndb) ` prompt, and
reads commands from stdin one per line; stdin EOF behaves like `q`.

## Command table

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

## Protocol for embedding front ends

`ndb --machine <module.nmod>` exposes the same session over a
tab-separated line protocol on stdin/stdout for embedding front ends —
the nide graphical debugger is built on it. Protocol details in
[Debugging in nide](../getting-started/debugging.md) and
[Debugger Architecture](../vm-architecture/debugging.md).
