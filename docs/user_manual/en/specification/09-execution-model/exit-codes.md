# 9.3 Program Entry and Exit

A program has one entry function: an explicit `int main()` or the
`int main()` synthesized from a script body (§3.2). Execution
begins by calling it; it takes no parameters.

The value the entry returns becomes the process exit status. The
implementation passes the entry's 32-bit return value to the host
unchanged; how the host presents it to an observer is host
behavior, not language behavior: a POSIX shell truncates the
value to its low 8 bits (a `return 300;` is observed as `44`),
while cmd, PowerShell and Python observe the full unsigned 32-bit
value (a `return -1;` is observed as `4294967295`). A program
whose exit status other tools inspect should stay within `0` to
`255`.

Reaching the end of the entry function returns the default value
of `int`, so the program exits with status `0` (§6.6). An
uncaught exception ends the program with status `1` (§8.1), and a
host-level run-time error (§7.1) likewise ends the program with
status `1`.

### What the tools return

*Informative:* `main`'s return value is the process exit code: nvm
(and the execution phase of `ncc run` and `ncc <file>`) ends the
process by handing that value to the operating system. nlang scripts
have no handwritten `main()`: the top-level `return` is the exit code,
because top-level statements are wrapped in a synthesized `main()`. A
program with its own `main()` runs under nlang unchanged (see
[nlang](../../cli-tools/nlang.md)). `ncc build` only compiles and never
executes; on success its exit code is always 0.

Different observers see different values for the same exit status:

| Observer | Value seen |
|--------|----------|
| Python `subprocess`, cmd's `%ERRORLEVEL%`, PowerShell's `$LASTEXITCODE` | the full unsigned 32-bit value |
| POSIX shell (bash `$?`, Git Bash, continuous integration (CI) bash steps) | the low 8 bits |

Negative return values are not recommended: the unsigned and the
truncated views are both surprising.

Failures of ncc, nvm, and nlang themselves always exit 1, kept
distinct from program exit codes:

- **ncc**: usage/argument errors, compilation failure (`Compilation
  failed.`), compiler internal errors (`Compiler internal error:`) → 1;
  `ncc build` success → 0; `ncc run`/direct-run mode finishing normally
  → `main`'s return value, runtime error → 1.
- **nvm**: module load failure, runtime error (including an uncaught
  NLang exception) → 1, printing `Runtime error: ...` and a call
  backtrace to stderr; normal completion → `main`'s return value.
- **nlang**: usage/argument errors, compilation failure, runtime errors
  (including uncaught exceptions) → 1 (same shape as nvm); normal
  completion → the top-level `return` value (or the handwritten
  `main()`'s return value); no top-level return → 0.
  **An empty script (no executable statements) exits 0.** This is
  deliberate and differs from nvm, which exits 1 for an artifact
  without an entry point: for an ad-hoc run, "nothing to do" is
  success.

The idioms follow from this: 0 means success; use `return 1;` for a
failed self-check (the Getting Started snippets' `if (condition) return
<sentinel value>; return 1;` uses this pattern); use distinct
small positive values to tell multiple failures apart.

### Test discipline

*Informative:* test expectations always use exit codes in **0–255**:
only then does the same program present the same value to every observer
(Python/cmd/PowerShell/bash). When a large number is needed, use modular
arithmetic or a derived value and encode "self-check passed" into a small
exit code — for example, after summing a loop, `if (total == 25) return
25; return 1;`.

See also [Getting Started / FAQ](../../getting-started/faq.md).
