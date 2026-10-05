# Module Serialization

A compiled unit image is distributed and loaded as a `.ncu` file: string
constants, functions, and the struct/class tables are serialized in a
fixed layout, and the loader reads them back with the same layout while
enforcing a minimum format version through the major/minor version. Multi-unit
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
resolves it to a table index by qualified name at load time. The
minimum format version is 2.0: the loader refuses every v1.x image outright, so
older modules must be recompiled.

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
