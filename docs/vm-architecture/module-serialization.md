# Module Serialization


Compiled modules are saved as `.nmod` files with this layout:

```text
"NLANGMOD"     magic (8 bytes)
uint16 majorVer = 1
uint16 minorVer = 10
string moduleName
string[] stringConstants
function[] functions
struct[] structs
class[] classes
```

**Version history**: v1.10 (array redesign B) — a semantic floor, not a
layout change: no new serialized fields, but array struct/class fields
now store `RTK_Array` as their `fieldTypeKinds` entry (previously the
element kind was stored), and the GC's field tracing and struct
streaming dispatch on that kind. A v1.9 module compiled by an older ncc
carries the old meaning, so the loader refuses minor < 10 outright —
older modules must be recompiled.
v1.9 (debugger) — each function record ends with a
`sourceFile` string (the TU path it was compiled from, after the locals
block); the import merge also copies `func.locals`, fixing a GC root-set
hole where imported frames had an empty root set.
v1.8 (Phase 13) — first-class function values:
`RTK_Func` kind byte plus the eight function-value opcodes above; older
VMs cannot execute these opcodes and refuse v1.8 modules outright (the
floor moved to minor 8 at this step).
v1.7 (Phase 11) — stdlib namespace intrinsics + reserved
namespaces; no field-layout change, but the relational string opcodes share
this version step, so older VMs must refuse these modules (the floor
moved to minor 7 at this step).

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[].

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[], fieldAccess[],
methodIndices[], constructorIdx.
