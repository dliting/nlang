# Debugging in nide

nide has a built-in debugger: breakpoints, stepping, and inspecting the
context list, call stack, and locals all happen inside the
integrated development environment (IDE). A debug session is
driven behind the scenes by `ndb --machine`, with the same semantics
as the [command-line ndb](../cli-tools/ndb.md); for the engine-side
layering see
[VM Architecture / Debugging Support](../vm-architecture/debugging.md).

Walk through the debugging flow with this program:

```nlang
import io;

int add(int a, int b) {
    return a + b;
}

int main() {
    int sum = 0;
    for (int i = 1; i <= 5; i = i + 1) {
        sum = add(sum, i);
    }
    io.print(sum);              // 15
    return 0;
}
```

Output `15`, exit code 0.

### Starting and stopping

1. Click the editor gutter next to the `for` line (or press F9) to set
   a breakpoint; a red dot appears in the gutter. The dot starts hollow
   and turns filled once the debug session confirms the line maps to
   the compiled code.
2. Choose Run → Start Debugging (F5): nide builds first, then starts
   the session; the program pauses at the first breakpoint, the output
   window switches to the Debug page, the paused line is highlighted in
   the editor, and a red arrow appears in the gutter.
3. Press F5 again to continue. The `for` header line is a loop anchor —
   the breakpoint hits on every iteration, matching gdb. The init,
   condition, and step segments of the header all count as the same
   breakpoint; arriving at any of them pauses, and the condition hits on
   every evaluation, including the final test before the loop ends.
4. Choose Run → Stop Debugging (Shift+F5) to end the session at any
   time. Stopping is a hard termination: the session stops immediately
   even in an infinite loop or inside a native call; closing nide also
   terminates the debugged process. While a session is active, Build
   and Run (Ctrl+F5) are disabled.

### Keyboard shortcuts

| Action | Shortcut |
|------|--------|
| Start Debugging (continue) | F5 |
| Run → Start (without debugging) | Ctrl+F5 |
| Stop Debugging | Shift+F5 |
| Toggle breakpoint | F9 (same as clicking the gutter) |
| Step Over | F10 |
| Step Into | F11 |
| Step Out | Shift+F11 |

### The Debug page

The Debug page of the output window concentrates the session state:

- session state (not debugging, paused, exited, ...);
- the Break on exceptions toggle: when checked, the throw point of any
  exception pauses the session (ndb's `catch on|off`) — at throw time
  the stack is not yet unwound, so the call stack and locals are still
  intact;
- the context tree (context / state / location): one row per concurrent
  context being debugged — the id, the scheduling state (suffixed
  `@<carrier>` while one carrier's execution window holds it), and the
  innermost frame location; clicking a row selects that context, with
  the semantics of [Multi-context debugging](#multi-context-debugging);
- the call stack tree (# / function / location): clicking a frame
  selects it, the editor jumps to the matching line, and the locals
  refresh;
- the locals tree (name / type / value): shows the locals of the
  selected frame in the context under inspection, limited to those
  whose declaration has been reached — a local
  joins the display once the paused statement is at or past its
  declaration line (on the declaration line itself it shows the default
  zero value until the initializer runs), and locals declared on later
  lines stay hidden; in a method frame the receiver shows as `this`,
  expanded one level (its fields).

Program output and the call-stack backtrace on error appear on the
Terminal page — a frame reads `at <function> (<source file>:<line>)`,
its line number coming from the statement anchors; frames of imported
units show the absolute path from the machine that built them.
Breakpoints are remembered by file path, survive
nide restarts, and follow a file automatically when it is renamed.

The program's standard input lives on the same page: while
a session is live, the terminal switches to local line editing — `> `
heads the pending input line (drawn locally by the IDE), and a typed
line, prompt included, is echoed to the terminal history and handed
to the debugged process only when you press Enter; the echo and the
line editing are synthesized locally by the IDE, not system-console
semantics (Ctrl+C is never injected as an interrupt; with a selection
it still copies), and the up/down arrows
recall recently typed lines (up to 50 within the session). Chinese
input goes through an input method (IME), with the pre-edit string
following the terminal cursor.

### Stepping

- Step Over (F10): finishes the current statement and stops at the next
  one — does not enter the callee;
- Step Into (F11): stops at the first statement of the called function;
- Step Out (Shift+F11): runs until the current function returns and
  stops at the caller's next statement.

Stepping acts on the context selected in the context tree (the stopped
context when no selection has been made); see
[Multi-context debugging](#multi-context-debugging).

The anchor lines of `while`/`for`/`do-while` hit breakpoints and steps
on every iteration, so even an empty loop body can be observed round by
round.

### Multi-context debugging

Contexts spawned by `thread.spawn` sit next to main in the context
tree, and a breakpoint hit or a thrown exception in any context stops
everything. Clicking a context row selects it as both the inspection
target and the step target (the same semantics as mainstream IDEs):

- the call stack and locals switch to that context's contents at once,
  and the editor jumps to its innermost frame location — parked
  contexts (a channel receive waiting for a sender, say) inspect like
  any other;
- the subsequent Step Over / Step Into / Step Out all act on that
  context: with a queued context selected, stepping stops at its next
  statement anchor; with a parked context selected, stepping lets the
  other contexts advance, and the selected one stops at its next
  statement once woken by data;
- selecting a parked context that never wakes and then stepping still
  trips the deadlock detector — stepping is not a way around a deadlock;
- every new stop redirects the selection back to the stopped context,
  so the tree marker never disagrees with where the program actually
  stopped.

In a single-context program the context tree holds only the main row,
and clicking it changes nothing; without a selection, stepping keeps
targeting the stopped context.

### Stripping debug info (`--no-debug-info`)

By default `ncc` emits a debug anchor before every statement (carrying
its source line) — backtrace line numbers, breakpoint binding, and
statement-level stepping all rest on the anchors. `ncc build
--no-debug-info` produces a module without them: a smaller artifact,
but backtraces lose their line numbers (frames print `(file:?)`),
breakpoints cannot bind, and statement-level stepping is gone; `ndb`
warns when it loads such a module. The remaining metadata — including
`sourceFile` — is kept, so function names and source paths still appear
in backtraces.

### Known limitations

- line-number drift is not tracked within a session: one session uses
  one line-number snapshot, and editing or rebuilding mid-session is
  unsupported; the next debug start re-resolves against the latest
  sources;
- Stop Debugging is a hard termination: the process ends immediately
  with no graceful unwinding (`finally` does not run);
- execution inside native code cannot be interrupted, but the process
  can always be killed;
- conditional breakpoints, watchpoints, edit-and-continue, and attaching
  to a running process are not yet supported.

See also: [VM Architecture / Debugging Support](../vm-architecture/debugging.md).
