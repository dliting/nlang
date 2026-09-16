# Exit Code Convention

## main return value → process exit code

`main`'s return value is the process exit code: nvm (and the execution
phase of `ncc run` and `ncc <file>`) ends the process by calling
`ExitProcess` with that value. `ncc build` only compiles and never
executes; on success its exit code is always 0.

Windows preserves the full 32-bit exit code, but **different observers
see different values**. Take `return 300;` as an example (all verified
in practice):

| Observer | Value seen |
|--------|----------|
| Python `subprocess` (the e2e runner), cmd's `%ERRORLEVEL%`, PowerShell's `$LASTEXITCODE` | 300 |
| POSIX shell (bash `$?`, Git-Bash, CI bash steps) | 44 (300 mod 256, i.e. the low 8 bits) |

Negative return values are not recommended: Windows interprets the value
as unsigned 32-bit (`return -1` is observed as 4294967295 in
Python/PowerShell/cmd) and POSIX shells truncate it on top — neither
view is intuitive.

## The tools' own 0/1 convention

Failures of ncc and nvm themselves always exit 1, kept distinct from
program exit codes:

- **ncc**: usage/argument errors, compilation failure (`Compilation
  failed.`), compiler internal errors (`Compiler internal error:`) → 1;
  `ncc build` success → 0; `ncc run`/direct-run mode finishing normally
  → `main`'s return value, runtime error → 1.
- **nvm**: module load failure, runtime error (including an uncaught
  NLang exception) → 1, printing `Runtime error: ...` and a call
  backtrace to stderr; normal completion → `main`'s return value.

The idioms follow from this: 0 means success; use `return 1;` for a
failed self-check (the Getting Started snippets' `if (condition) return
<sentinel value>; return 1;` guard has exactly this shape); use distinct
small positive values to tell multiple failures apart.

## Test discipline

- Test expectations always use exit codes in **0–255**: only then does
  the same program present the same value to every observer
  (Python/cmd/PowerShell/bash). The second column of the e2e manifest
  (`tests/e2e/manifest.txt`) is exactly that expected exit code.
- When a large number is needed, use modular arithmetic or a derived
  value and encode "self-check passed" into a small exit code — for
  example, after summing a loop, `if (total == 25) return 25; return 1;`.

See also: [Getting Started / FAQ](../getting-started/faq.md).
