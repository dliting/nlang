# ndisasm — Disassembler

ndisasm turns a `.ncu` back into a readable bytecode dump — the
answer to "what did the compiler actually generate?". Use it to
cross-check instruction addresses against ndb's `x` output, inspect
import slots before load-time linking, and diagnose serialization or
module-loading problems.

```text
ndisasm <module.ncu | package.npkg>
ndisasm -func <name> <module.ncu | package.npkg>
```

Two invocations: a full dump, or `-func <name>` keeping only one
function section (the other sections remain). **`-func` must precede
the module path** — after it, the flag is silently ignored and the
output equals the full dump. `-func <name>` matches the function-table
key verbatim: the key of a user function is module- or
package-qualified (e.g. `hello.main`), so a bare name matches nothing —
no error is reported, the function section is simply absent. The input
may also be a `.npkg` package
archive: a `package:` line comes first, then each member image is
dumped in member-table order (every member carries its own full set of
sections); a corrupt member reports an error without aborting the dump
of its siblings, and the exit code reflects the failure. Within one
image the output sections come in a fixed order:
`module:` → `structs:` → `classes:` → `string constants:` → one
section per function.

The function header line carries all metadata (`file=` mirrors the
source path form passed at compile time; built-in functions carry
`intrinsic=N` and no `file=`; native-bound functions carry `native`):

```text
function hello.main (frameSize=28, params=0, returnType=i32, file=examples/hello.n)
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
dozens even for a hello), so day-to-day inspection of one
function uses the `-func` filter.
