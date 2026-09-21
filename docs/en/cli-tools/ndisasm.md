# ndisasm — Disassembler

ndisasm turns a `.nmod` back into a readable bytecode dump — the
answer to "what did the compiler actually generate?". Use it to
cross-check instruction addresses against ndb's `x` output, review
optimization results, and diagnose serialization or module-loading
problems.

```text
ndisasm <module.nmod>
ndisasm -func <name> <module.nmod>
```

Two invocations: a full dump, or `-func <name>` keeping only one
function section (the other sections remain). **`-func` must precede
the module path** — after it, the flag is silently ignored and the
output equals the full dump. The output sections come in a fixed order:
`module:` → `structs:` → `classes:` → `string constants:` → one
section per function.

The function header line carries all metadata (`file=` mirrors the
source path form passed at compile time; built-in functions carry
`intrinsic=N` and no `file=`; native-bound functions carry `native`):

```text
function main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
  bytecode:
    0000: debug 2
    0003: const_i32 42
    0008: assign 0
    ...
```

When a function contains try blocks, a `try blocks:` exception-table
dump follows the instruction list (one `[pc range) handler=...
class=... catchLocal=...` line per block).

A full dump lists every built-in class method (all `(no bytecode)` —
fifty-seven even for a hello), so day-to-day inspection of one
function uses the `-func` filter.
