# Debugging Support

NLang ships **ndb**, a CLI debugger (`ndb <module.nmod>`). It loads the
module in-process, runs it on the standard `VmExecutor`, and drives the
VM through two small interfaces — the same engine layer the nide
debugger reuses over a line protocol (the debugpy/dlv "engine + thin
front ends" model).

## Hook model

- `IDebugHooks` (src/vm/IDebugHooks.h) — the callback face. Two
  checkpoints:
  - `OnStatement` fires after every `OP_DebugInfo` (each statement is
    preceded by one, carrying its source line).
  - `OnThrow` fires at every raise site (built-in failures in
    `RaiseNlangException`, user `throw` and `rethrow` at their opcodes)
    after the exception object exists but **before** unwinding starts —
    the full NLang call stack and every frame's locals are still alive.
- `IVmDebugView` — read-only queries over the frozen state: frame
  count, per-frame function name/source file/line/pc, and per-frame
  locals with display strings.

Installing hooks (`VmExecutor::SetDebugHooks`) is optional; with no
front end installed the checkpoints cost one null test per statement.

## Stop semantics

The callback runs with the program frozen (NLang function calls are
C++ recursion, so the whole stack lives inside `OnStatement`). Not
returning keeps it frozen; returning resumes in place. ndb's command
loop runs inside the callback.

Statement granularity: one stop per statement. Two statements on the
same line stop twice; a statement spanning lines stops once.

## Loop anchors hit every iteration

The loop statements carry their debug anchor on the back edge, so a
breakpoint on the anchor line — or stepping — stops on **every**
iteration, not only at loop entry (gdb semantics): the condition entry
for `while`/`for` — evaluated once per iteration, including the final
evaluation that ends the loop — and the tail condition for `do-while`.
Previously the anchor fired once at loop entry, leaving an empty-bodied
loop without a per-iteration checkpoint. The anchor is the statement
line the grammar records for the loop header, so `b <file>:<loop line>`
behaves the way line-oriented debuggers have always behaved; a `for`
header also carries its init and increment statements on that line, and
each of those anchors stops under the line's single breakpoint id.

## Session layering

Between the engine hooks and the front ends sits
`DebugSessionController` (src/vm/DebugSessionController.h, PRIVATE
include like the hooks): the front-end-agnostic session state — the
breakpoint table, function breakpoints, source-path matching and the
step-depth state machine. The executor calls it through `IDebugHooks`;
front ends implement `IDebugFrontEnd` (OnStopped / WaitUntilResume /
OnExited / OnRuntimeError) and are driven through `StopInfo` payloads.
Two adapters ship: ndb's interactive CLI and `--machine`. The frozen
window opens at `OnStopped` and closes when `WaitUntilResume` returns;
resume commands and the debug view outside that window are a front-end
programming error (`std::logic_error`).

Breakpoint identity is **one id per source line**: a line carrying
several statement anchors (e.g. two statements written on one line)
merges them under a single breakpoint id, so set/delete/list are
unambiguous and identical across both front ends. Statement-level
granularity is unchanged — every anchor under the id still stops; the
stop reports the shared id.

## Machine mode

`ndb --machine <module.nmod>` speaks a line protocol on stdin/stdout
(documented in full in src/tools/ndb/MachineFrontEnd.h): events are
tab-joined lines with escaped fields
(`hello`/`bp`/`stopped`/`frame`/`local`/`done`/`output`/`exited`/
`error`/`err`), commands are plain space-separated tokens
(`b`/`bfunc`/`d`/`breakthrow`/`bt`/`frame`/`locals`/`run`/`c`/`s`/
`n`/`f`). The session opens with a prelude in which breakpoints are
preset; `run` ends the prelude and starts execution — before it,
window-bound commands answer `err` (nothing is frozen yet). A resume
command answers with the next stop or exit event; other commands answer
in place. Frame numbering: `stopped`'s depth is 1-based and always
equals the frame count (a stop freezes the innermost frame), while
`bt`/`frame` index 0-based, innermost = 0.

## Host I/O seam

`IHostIo` (src/vm/IHostIo.h) decouples the executor's I/O from the
process console: output bytes arrive verbatim through `OnOutput`, and
input is opt-in — an installed host that does not override
`IsInputAvailable()` makes `io.readLine` raise a catchable IOException
instead of silently consuming the embedder's stream. Machine mode
implements the seam to route program output into `output` events while
keeping stdin as the protocol channel; with no host installed (nvm,
ncc, the CLI front ends) behavior is unchanged. `OnOutput` must not
throw: it runs on the execution thread, inside the same freeze-time
discipline as the hooks.

## Freeze-time discipline

Inside a callback:

- never execute NLang code (no `toString` dispatch — formatters are
  shallow, one level of fields/elements with short tags for nested
  references);
- never allocate on the NLang heap (the heap is consistent at freeze
  time and must stay that way — C++ allocation is fine, the collector
  only runs at safepoints during execution);
- never let C++ exceptions escape into the VM (they would cross the
  NLang try/catch boundary) — ndb's command loop catches everything.

Reference-typed values are discriminated like the GC marker does:
declared kind prunes primitives; array-typed fields carry the
declaration-side `RTK_Array` in `.nmod` (array redesign B), so the
declared kind is the reliable array detector and the runtime slot kind
corroborates it. Only Class/Struct/Func declared kinds fall through to
the runtime slot kind; Int32/Float/String/Array render directly from
the declared kind, so a plain int never reaches the ref-tag path.

## Breakpoint addressing (`.nmod` v1.9)

Each function records the path of the translation unit it was compiled
from (`CompiledFunction::sourceFile`). `b file.n:LINE` suffix-matches
recorded paths, `b LINE` resolves in the selected frame's file,
`b funcName` stops at the function's first statement. The import merge
copies `sourceFile` and `locals`, so imported functions are
breakpoint-addressable and their frames inspectable.

## Known limits (v1)

No conditional breakpoints or watchpoints; empty-bodied `b func` never
hits; function names render bare (no `Class.method` qualification); the
exception object is not exposed as a pseudo-variable at the throw stop;
no attach to running processes; native calls step through transparently;
throw stops anchor at the statement's pc approximation; pc values are
16-bit bytecode offsets (inherited from the executor's existing
`uint16_t opPc`, Phase 9d precedent — a function with >64 KiB of
bytecode would wrap; a pre-existing VM bound, not a debugger limit); a
shared `.nmod` may carry stale source paths (ndb falls back to the
`.nmod`'s directory, then degrades `l` to numbers-only). The IDE
session inherits these and adds user-facing ones — no stdin inside a
debug session, per-session line-number snapshots (no mid-session
edit/rebuild), hard-terminate stop — documented in the Getting Started
debugging guide, [在 nide 中调试](../getting-started/debugging.md).
