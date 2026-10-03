# nvm — Run a Program

nvm runs an already-compiled artifact — a `.ncu` unit image or a `.npkg`
program archive — with no compile step. Use it to execute without
recompiling in deployed or automated environments, to run the same
program repeatedly, and as the execution step in scripts and pipelines
where the exit code decides the outcome.

```text
nvm <program.ncu|.npkg> [-I <dir>...] [--gc-stress=N]
```

Runs a compiled program; the process exit code is `main`'s return value
(conventions on the [exit code page](../language-spec/exit-code-convention.md)).
An artifact file that cannot be opened reports
`Runtime error: nloader failed:` (the diagnostic body lists the problems
one per line, e.g. `'<path>': Failed to open module file: <path>`).
`--gc-stress=N` is a testing knob: it clamps both GC thresholds to tiny
values so any untraced reference goes stale within a few allocations —
useful for verifying memory-management changes, not needed in everyday
use. The flag may appear before or after the program path.

The artifact itself carries no library code: the loader first discovers
every dependency along the search path (the standard-library archive,
external `.ncu`/`.npkg`), then links them into the one runtime module
and executes it. A missing dependency reports
`Runtime error: nloader failed:` with a body line like
`module '...' not found (searched: <dir>, ...)` listing every directory
searched — check `-I` and `NLANG_PATH` first (the mechanism is covered
in [ncc](ncc.md), "Artifacts and load-time linking").

`-I <dir>` appends a library search directory (repeatable). At run time
closure members and native dynamic libraries are located across ordered
directories: `-I` dirs → the program's directory → the `NLANG_PATH`
environment variable (`;` on Windows, `:` on POSIX) → the executable
directory / current directory; earlier dirs win and duplicates keep only
the first. See "Libraries and search paths" in the language specification
for the full rules.
