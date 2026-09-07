# Module Serialization


Compiled modules are saved as `.nmod` files with this layout:

```text
"NLANGMOD"     magic (8 bytes)
uint16 majorVer = 1
uint16 minorVer = 9
string moduleName
string[] stringConstants
function[] functions
struct[] structs
class[] classes
```

**Version history**: v1.9 (debugger) — each function record ends with a
`sourceFile` string (the TU path it was compiled from, after the locals
block); the import merge also copies `func.locals`, fixing a GC root-set
hole where imported frames had an empty root set. The loader floor is
minor 9.
v1.8 (Phase 13) — first-class function values:
`RTK_Func` kind byte plus the eight function-value opcodes above; older
VMs cannot execute these opcodes and refuse v1.8 modules outright (the
loader floor is minor 8).
v1.7 (Phase 11) — stdlib namespace intrinsics + reserved
namespaces; no field-layout change, but the relational string opcodes share
this version step, so older VMs must refuse these modules (the loader floor
is minor 7).

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[].

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[], fieldAccess[],
methodIndices[], constructorIdx.
