# nvm — Run a Program

nvm runs an already-compiled artifact — a `.ncu` unit image or a `.npkg`
program archive — with no compile step. Use it to execute without
recompiling in deployed or automated environments, to run the same
program repeatedly, and as the execution step in scripts and pipelines
where the exit code decides the outcome.

```text
nvm <program.ncu|.npkg> [-I <dir>...] [--verbose | -v] [--gc-stress=N] [--carriers=N]
```

Runs a compiled program; the process exit code is `main`'s return value
(conventions on the [exit code page](../language-spec/exit-code-convention.md)).
An artifact file that cannot be opened reports
`Runtime error: nloader failed:` (the diagnostic body lists the problems
one per line, e.g. `'<path>': Failed to open module file: <path>`).
`--gc-stress=N` is a testing knob: it clamps both garbage collection (GC) thresholds to tiny
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
all dependency members and native dynamic libraries are located across
ordered directories: `-I` dirs → the program's directory → the `NLANG_PATH`
environment variable (`;` on Windows, `:` on POSIX) → the executable
directory / current directory → the standard-library directory (where
`stdlib.npkg` lives); earlier dirs win and duplicates keep only the
first. See
["Libraries and search paths"](../language-spec/stdlib-search-paths.md)
for the full rules.

## Concurrency and carriers

`--carriers=N` sets how many operating-system threads execute the
program's virtual threads (1..64; out-of-range values are rejected).
Omitted, nvm uses the smaller of 4 and the machine's hardware
concurrency. `--carriers=1` is fully deterministic: a strict
first-in-first-out rotation, so the same program with the same inputs
produces the same interleaving every run — tests may assert on it.
With more than one carrier the interleaving is unspecified: program
correctness must rest on the synchronization primitives (`join`,
`Channel`, `Mutex`, `Future.get`), never on timing. The carrier count
changes interleaving, not results:

```text
nvm --carriers=1 program.ncu    # reproducible interleaving
nvm --carriers=8 program.ncu    # same results, different interleaving
```

See [Carriers and
Parallelism](../language-spec/concurrency-carriers.md) for what
multiple carriers actually parallelize; the shipped `examples/conc_*.n`
demonstrate the flag.

`--verbose` (short form `-v`) prints the resolved search path before
the program runs —
one directory per line, in search order, each line annotated with the
layer it came from (`(-I)`, `(local directory)`, `(NLANG_PATH)`,
`(system)`) — then executes normally. When the not-found diagnostic above fires, rerun the failing
invocation with `--verbose` to see every directory that will be
searched.
