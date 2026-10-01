# Module Serialization

The compiled module is distributed and loaded as a `.nmod` file: string
constants, functions, and the struct/class tables are serialized in a
fixed layout, and the loader reads them back with the same layout while
enforcing a semantic floor through the minor version. To inspect the
contents of a `.nmod` directly, you can disassemble it on the command
line with [ndisasm](../cli-tools/ndisasm.md).

Compiled modules are saved as `.nmod` files with this layout:

```text
"NLANGMOD"     magic (8 bytes)
uint16 majorVer = 1
uint16 minorVer = 14
string moduleName
string[] stringConstants
function[] functions
struct[] structs
class[] classes
```

**Version history**: v1.14 (debugger local scope) — a layout change:
each local-variable descriptor in a function's locals block gains a
`declPc` uint16 (the bytecode offset where the local's declaration is
reached; 0 for parameters and the implicit method receiver). The debug
views use it to hide locals whose declaration the paused position has
not reached yet. A v1.13 module from an older ncc lacks those two bytes
per local, so the loader refuses minor < 14 outright — older modules
must be recompiled.
v1.13 (primitive-type family expansion) — a semantic
floor, not a layout change: no new serialized fields, but the scalar kind
code space is expanded — byte/ubyte/short/ushort/uint/long/ulong/double/
bool/char appear with new RTK codes (10..19) in the kind bytes of locals
and fields, in type descriptors, and as boxing tags; numeric instructions
are emitted as generic families with a kind immediate. A v1.12 module from
an older ncc lacks the semantics of these kinds, so the loader refuses
minor < 13 outright — older modules must be recompiled.
v1.12 (recursive type descriptors) — a layout
change: each function record gains a formal-parameter type descriptor
sequence, and each struct/class field gains a field type descriptor
(recursive type descriptors: nested arrays, `List`/`Dict`
instantiations, struct/class indices, depth cap 8), recording the real
formal, return, and field types. Imported function stubs are rebuilt
with their real signatures (not return-kind placeholders), so
cross-module call-site type checking is consistent with same-module
calls; a `lib.mk()` returning `float[]` assigned to an `int[]` local is
rejected, float-argument widening matches same-module calls exactly,
and `out` arguments round-trip. A v1.11 module from an older ncc lacks
the descriptors, so the loader refuses minor < 12 outright — older
modules must be recompiled.
v1.11 (generic array type arguments) — a semantic
floor, not a layout change: no new serialized fields, but array-typed
elements of generic containers (`List<T[]>`, `Dict` keys/values) now
flow as raw array handles with no boxing, and `foreach` loop variables
over them occupy `RTK_Array` local slots the GC traces. A v1.10 module
from an older ncc boxes those elements into primitive slots the GC
never traces, so the loader refuses minor < 11 outright — older
modules must be recompiled.
v1.10 (array field type tags) — a semantic floor, not a
layout change: no new serialized fields, but array struct/class fields
now store `RTK_Array` as their `fieldTypeKinds` entry (previously the
element kind was stored), and the GC's field tracing and struct
streaming dispatch on that kind. A v1.9 module compiled by an older ncc
carries the old meaning, so the loader refuses minor < 10 outright —
older modules must be recompiled.
v1.9 (debugger) — each function record ends with a
`sourceFile` string (the TU path it was compiled from, after the locals
block); the import merge also copies `func.locals`, so imported frames
have a complete GC root set.
v1.8 — first-class function values:
`RTK_Func` kind byte plus the eight function-value opcodes above; older
VMs cannot execute these opcodes and refuse v1.8 modules outright (the
floor moved to minor 8 at this step).
v1.7 — stdlib namespace intrinsics + reserved
namespaces; no field-layout change, but the relational string opcodes share
this version step, so older VMs must refuse these modules (the floor
moved to minor 7 at this step).

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[].

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[], fieldAccess[],
methodIndices[], constructorIdx.
