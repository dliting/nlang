# ndb — Debugger

ndb is NLang's interactive debugger: it loads a program, stops at the
entry point, and follows commands for breakpoints, stepping, locals,
and the call stack. Reach for it when a program misbehaves; debugging
cross-file projects (breakpoints by `file:line` or function name) is
the primary use case. The same session is also exposed to embedding
front ends as a line protocol (`--machine`) — nide's graphical
debugger is built on it.

```text
ndb <program.ncu|.npkg> [-I <dir>...] [--verbose | -v]
```

After loading the program it **stops at the first statement of the
entry point** (like gdb's `start`), prints the `(ndb) ` prompt, and
reads commands from stdin one per line; stdin end of file (EOF) behaves like `q`.

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
| `threads` | — | List all contexts (state/location) |
| `t <id>` | thread | Switch the inspection and step-target context |
| `bt` | backtrace | Call stack |
| `frame <n>` | — | Select a frame |
| `info locals` | info | Locals of the selected frame (synthesized names filtered; the receiver shows as `this`) |
| `p <name>` | print | Print one local |
| `l [line]` | list | Source window (current line marked `->`) |
| `x` | — | Disassembly of the selected frame (current instruction marked `>>`) |
| `catch on\|off` | — | Break on throw (default off) |
| `help` | — | Command help |
| `q` | quit | Quit (kills the debugged program) |

The short forms are the canonical command surface (matching the `help`
output); long aliases are accepted as equivalents.

A complete session (debugging `examples/hello_project`, breakpoint set
by qualified function name in the second file):

```console
$ ncc build -p examples/hello_project/hello_project.nproj -o hello_project.npkg
Output package file: hello_project.npkg ...
Compiled successfully: hello_project.npkg
$ ndb hello_project.npkg
Stopped: main.main (main.n:6)
(ndb) b utils.addBoth
Breakpoint 1 at utils.addBoth (utils.n:2)
(ndb) c
Breakpoint 1, utils.addBoth (utils.n:2)
(ndb) bt
#0  utils.addBoth (utils.n:2)
#1  main.main (main.n:6)
(ndb) info locals
a = 40
b = 2
(ndb) p a
a = 40
(ndb) c
Program exited with code 0.
```

(Function names are qualified by module path — `b utils.addBoth`; the
spelling rules are in [Packages](../language-spec/packages.md) in the
language specification.)

When the program finishes, ndb prints `Program exited with code N.` and
exits with that same code; `q` or stdin EOF kills the program and exits
ndb itself with 0.

## Multi-context debugging

Contexts spawned by `thread.spawn` are equally under the debugger's
control: a breakpoint hit or a throw in any context stops everything —
after the stop no virtual thread starts executing, and each in-flight
segment finishes and then halts. `threads` prints one row per context:
the focus marker (`*`), the id, the scheduling state (suffixed
`@<carrier>` while a carrier — the operating-system thread running
it — executes it; this is where running-now differs from queued
runnable), and the innermost frame
location; a finished context has no frames and shows only the id and
state. `t <id>` moves
the inspection focus to that context — `bt`/`frame`/`info locals`/`p`/
`l`/`x` then address it, and suspended frames (a channel receive waiting
for a sender, say) inspect like any other; a context still held by a
carrier (`@<carrier>`) keeps its frames on that carrier's execution
stack, which is not inspectable while stopped — `bt` and the locals
query report no frames for it until it suspends or finishes; each stop
resets the focus back to the stopped context. `t` also sets the step
target (as mainstream integrated development environments do): the subsequent
`s`/`n`/`f` act on the selected context — stepping with a suspended
context selected lets the other contexts advance, and the selected one
stops at its next statement once woken; selecting a suspended context
that never wakes still trips the deadlock detector. Without a `t`
selection, stepping keeps targeting the stopped context.

```console
$ ndb dbg_threads.ncu
Stopped: dbg_threads.main (dbg_threads.n:11)
(ndb) b 6
Breakpoint 1 at run (dbg_threads.n:6)
(ndb) c
Breakpoint 1, run (dbg_threads.n:6)
(ndb) threads
  0  parked-chan-recv  dbg_threads.main (dbg_threads.n:14)
* 1  runnable@0  run (dbg_threads.n:6)
(ndb) t 0
* 0  parked-chan-recv  dbg_threads.main (dbg_threads.n:14)
(ndb) bt
#0  dbg_threads.main (dbg_threads.n:14)
(ndb) c
Program exited with code 9.
```

(The breakpoint sits in `run`, which a spawned context executes; when
it hits, main is suspended in a `recv` on an empty channel — `threads`
lists it as `parked-chan-recv` while `run` is `runnable@0` (held by
carrier 0). After `t 0` moves the focus to main, `bt` reads its suspended
frame.)

## Protocol for embedding front ends

`ndb --machine <program.ncu|.npkg> [-I <dir>...] [--verbose | -v]` exposes
the
same session over a tab-separated line protocol on stdin/stdout for
embedding front ends — the nide graphical debugger is built on it.
For using the graphical front end, see
[Debugging in nide](../getting-started/debugging.md); for the protocol
itself, see [Debugger Architecture](../vm-architecture/debugging.md).

Both the interactive and `--machine` forms accept `-I <dir>` (repeatable)
to locate all dependency members and native dynamic libraries; the search dirs
are assembled in the same order as nvm: `-I` → program directory →
`NLANG_PATH` → executable directory / current directory → the
standard-library directory (where `stdlib.npkg` lives).

`--verbose` (short form `-v`) prints the resolved search path (one
directory per line,
annotated with its layer, as in nvm) before the session starts, then
proceeds normally. In `--machine` mode the listing goes to **stderr** —
stdout is the protocol channel and must stay free of non-protocol
lines.
