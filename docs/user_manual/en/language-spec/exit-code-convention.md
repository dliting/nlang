# Exit Code Convention

The normative rules are in [specification §10.3](../specification/10-execution-model.md).
## main return value → process exit code

`main`'s return value is the process exit code: nvm (and the execution
phase of `ncc run` and `ncc <file>`) ends the process by calling
`ExitProcess` with that value. nlang scripts have no handwritten
`main()`: the top-level `return` is the exit code, because top-level
statements are wrapped in a synthesized `main()`. A program with its
own `main()` runs under nlang unchanged (see
[nlang](../cli-tools/nlang.md)). `ncc build` only compiles and never
executes; on success its exit code is always 0.

Windows preserves the full 32-bit exit code, but **different observers
see different values**. Take `return 300;` as an example:

| Observer | Value seen |
|--------|----------|
| Python `subprocess`, cmd's `%ERRORLEVEL%`, PowerShell's `$LASTEXITCODE` | 300 |
| POSIX shell (bash `$?`, Git-Bash, continuous integration (CI) bash steps) | 44 (300 mod 256, i.e. the low 8 bits) |

Negative return values are not recommended: Windows interprets the value
as unsigned 32-bit (`return -1` is observed as 4294967295 in
Python/PowerShell/cmd) and POSIX shells truncate it on top — neither
view is intuitive.

## The tools' own 0/1 convention

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

## Test discipline

- Test expectations always use exit codes in **0–255**: only then does
  the same program present the same value to every observer
  (Python/cmd/PowerShell/bash).
- When a large number is needed, use modular arithmetic or a derived
  value and encode "self-check passed" into a small exit code — for
  example, after summing a loop, `if (total == 25) return 25; return 1;`.

See also: [Getting Started / FAQ](../getting-started/faq.md).
