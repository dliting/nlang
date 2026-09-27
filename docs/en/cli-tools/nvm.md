# nvm — Run a Module

nvm runs an already-compiled `.nmod` module — no compile step. Use it
to execute without recompiling in deployed or automated environments,
to run the same module repeatedly, and as the execution step in
scripts and pipelines where the exit code decides the outcome.

```text
nvm <module.nmod> [-I <dir>...] [--gc-stress=N]
```

Runs a compiled module; the process exit code is `main`'s return value
(conventions on the [exit code page](../language-spec/exit-code-convention.md)).
A module that cannot be opened reports
`Runtime error: Failed to open module file: <path>`. `--gc-stress=N` is
a testing knob: it clamps both GC thresholds to tiny values so any
untraced reference goes stale within a few allocations — useful for
verifying memory-management changes, not needed in everyday use. The
flag may appear before or after the module path.

`-I <dir>` appends a native-library search directory (repeatable). At run
time native dynamic libraries are loaded across ordered directories:
`-I` dirs → the module directory → the `NLANG_PATH` environment variable
(`;` on Windows, `:` on POSIX) → the executable directory / current
directory; earlier dirs win and duplicates keep only the first. See
"Libraries and search paths" in the language specification for the full
rules.
