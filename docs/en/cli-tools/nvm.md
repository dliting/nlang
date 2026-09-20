# nvm — Run a Module

```text
nvm <module.nmod> [--gc-stress=N]
```

Runs a compiled module; the process exit code is `main`'s return value
(conventions on the [exit code page](../language-spec/exit-code-convention.md)).
A module that cannot be opened reports
`Runtime error: Failed to open module file: <path>`. `--gc-stress=N` is
a testing knob: it clamps both GC thresholds to tiny values so any
untraced reference goes stale within a few allocations — useful for
verifying memory-management changes, not needed in everyday use.
