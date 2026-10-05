# Module Serialization

A compiled unit image is distributed and loaded as a `.ncu` file: string
constants, functions, and the struct/class tables are serialized in a
fixed layout, and the loader reads them back with the same layout while
enforcing a format floor through the major/minor version. Multi-unit
programs and libraries ship as `.npkg` package archives (end of this
page). To inspect an artifact's contents directly, disassemble it on
the command line with [ndisasm](../cli-tools/ndisasm.md).

Compiled unit images are saved as `.ncu` files with this layout:

```text
"NLANGCU "    magic (8 bytes, NUL-padded)
uint16 majorVer = 2
uint16 minorVer = 1
string modulePath        v2.0: this unit's dotted module path (package identity)
string moduleName
string entryKey          v2.0: the entry function's qualified name; empty = no entry
string[] stringConstants
function[] functions
struct[] structs
class[] classes
arrayType[] arrayTypes
enumNames[] / enumKeys[] two parallel enum tables
importSlots × 4          v2.0: the function/class/struct/enum import-slot tables
```

Each of the four import-slot tables is a list of "module path + name +
parameter count" entries (the function table also carries an
ownerClassKey, empty = namespace-level function). The own entries
occupy each table's prefix indices 0..n-1 and the import slots are
appended after them (n..n+m) — a cross-unit call's operand shares the
same index space with own calls, the linker remaps every operand
uniformly, and no opcode distinction is needed. The entry is no longer
serialized as an index: the program package's entry record (in the
`.npkg` header) or the bare unit's `<modulePath>.main` convention
resolves it to a table index by qualified name at load time.

**Version history**: v2.1 (merged development lines) — no layout
change: the content-level changes from the parallel line — the
12-primitive scalar-kind table and the per-local declaration-scope
field — ride on the v2.0 layout. The loader refuses any older image
outright: every v1.x module and every pre-merge 2.0 module must be
recompiled.
v2.0 (load-time linking) — a layout change: the
header gains the module's dotted path and the entry qualified name, and
drops v1.13's `int32 entryPoint`; the enum qualified-key table and the
four import-slot tables arrive with the per-unit artifacts. Artifacts
no longer merge library code; cross-unit symbols resolve at link time.
The floor rises to 2.0 as a whole: the loader refuses every v1.x image
up front by the version check (its layout would misparse at the first
new field anyway) — older modules must be recompiled.
v1.13 (qualified keys) — a layout
change: struct/class/function table keys and stream type-name literals
become package-qualified (`<package>.<name>`; ownerless built-ins keep
the bare name), and the wire gains an `int32 entryPoint` field (index
of the entry function, -1 when the module exports none) right after the
module name. A v1.12 module misparses every keyed name, so the loader
refuses minor < 13 outright — older modules must be recompiled.
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
the descriptors, so the loader refuses minor < 13 outright — older
modules must be recompiled.
v1.11 (generic array type arguments) — a semantic
floor, not a layout change: no new serialized fields, but array-typed
elements of generic containers (`List<T[]>`, `Dict` keys/values) now
flow as raw array handles with no boxing, and `foreach` loop variables
over them occupy `RTK_Array` local slots the garbage collection (GC) traces. A v1.10 module
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
`sourceFile` string (the translation unit (TU) path it was compiled from, after the locals
block); merged-in functions keep their `locals`, so imported frames
have a complete GC root set.
v1.8 — first-class function values:
`RTK_Func` kind byte plus the eight function-value opcodes above; older
VMs cannot execute these opcodes and refuse v1.8 modules outright (the
floor moved to minor 8 at this step).
v1.7 — stdlib intrinsics + reserved library
names; no field-layout change, but the relational string opcodes share
this version step, so older VMs must refuse these modules (the floor
moved to minor 7 at this step).

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[], fieldTypeDescs[].

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[],
fieldTypeDescs[], fieldAccess[], methodIndices[], constructorIdx.

## Package archives (.npkg, format 1.0)

Multi-unit programs and libraries ship as package archives: a header
(package name, format version, flags, a reserved signature-block
descriptor, and — for program packages — the **entry record**: the
entry member's module path plus the function name), a member table
(module path → offset/length/checksum, sorted by path for byte-level
determinism), and the embedded `.ncu` unit images. Each member carries
an FNV-1a 64 checksum (against accidental corruption — not a security
mechanism). A library package is the same container without an entry
record — the standard library's `stdlib.npkg` is a library package
whose members are `io`/`math`/`fs`. The loader requires each member
image's header module path to match its member-table entry: a
mislabeled artifact is refused, not silently accepted.
