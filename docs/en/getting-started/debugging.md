# Debugging in nide

nide has a built-in debugger: breakpoints, stepping, and inspecting the
call stack and locals all happen inside the IDE. A debug session is
driven behind the scenes by `ndb --machine`, with the same semantics as
the command-line ndb; for the engine-side layering see
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
   time. Stopping is a hard terminate: even an infinite loop or a stop
   inside a native call ends immediately; closing nide also terminates
   the debugged process. While a session is active, Build and Run
   (Ctrl+F5) are disabled.

### Keyboard shortcuts

| Action | Shortcut |
|------|--------|
| Start Debugging / Continue | F5 |
| Run without debugging | Ctrl+F5 |
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
- the call stack tree (# / function / location): clicking a frame
  selects it, the editor jumps to the matching line, and the locals
  refresh;
- the locals tree (name / type / value): shows every local of the
  selected frame.

Program output and the call-stack backtrace on error appear on the Run
Output page. Breakpoints are remembered by file path, survive nide
restarts, and follow a file automatically when it is renamed.

### Stepping

- Step Over (F10): finishes the current statement and stops at the next
  one — does not enter the callee;
- Step Into (F11): stops at the first statement of the called function;
- Step Out (Shift+F11): runs until the current function returns and
  stops at the caller's next statement.

The anchor lines of `while`/`for`/`do-while` hit breakpoints and steps
on every iteration, so even an empty loop body can be observed round by
round.

### Known v1 limitations

- Debug sessions have no stdin: `io.readLine` throws an `IOException`
  (catchable with try/catch) instead of hanging silently;
- line-number drift is not tracked within a session: one session uses
  one line-number snapshot, and editing or rebuilding mid-session is
  unsupported; the next debug start re-resolves against the latest
  sources;
- Stop Debugging is a hard terminate: the process ends immediately with
  no graceful unwinding (`finally` does not run);
- execution inside native code cannot be interrupted, but the process
  can always be killed;
- conditional breakpoints, watchpoints, edit-and-continue, and attaching
  to a running process are not yet supported.

See also: [VM Architecture / Debugging Support](../vm-architecture/debugging.md).
