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
uint16 minorVer = 7
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

Each of the four import-slot tables is a list of entries carrying the
target's module path and the qualified name; the function table
additionally carries the parameter count, an owner key (empty = a
namespace-level function, otherwise the owning class's or struct's
qualified key), a static flag, an owner kind (0 namespace-level, 1
class member, 2 struct member), and a constructor ordinal (0xFFFF = not
a constructor, else the index into the owner's constructorIndices).
The own entries
occupy each table's prefix indices 0..n-1 and the import slots are
appended after them (n..n+m) — a cross-unit call's operand shares the
same index space with own calls, the linker remaps every operand
uniformly, and no opcode distinction is needed. The entry is no longer
serialized as an index: the program package's entry record (in the
`.npkg` header) or the bare unit's `<modulePath>.main` convention
resolves it to a table index by qualified name at load time. The
minimum format version only rises with the toolchain (currently 2.8;
from 2.2 the type descriptors carry `func` signatures, from 2.3
default-value expressions serialize with the image — imported stubs
bind the formal names the function records always carried, from 2.4
function records gain a static flag and class records gain the exported
public-static method indices, from 2.5 interface records gain a
constants block, from 2.6 struct records gain a per-field defaults
block and constructor-index segments and function import records gain
an owner kind and a constructor ordinal, from 2.7 class records carry
their own declared fields only plus per-method access and virtuality
bytes and per-constructor access (the exported public-static method
index block is removed — staticness rides the function record's static
flag), struct records gain per-constructor access, and interface
records gain an instance-method signature block, and from 2.8 function
records gain a trailing eval-area handle mask (the garbage
collector's per-slot root classification over the frame band)):
the loader refuses
any image below the current version, so older modules must be
recompiled.

Each struct includes: name, fieldCount, fieldNames[], fieldTypeKinds[],
fieldStructIndices[], fieldClassIndices[], fieldTypeDescs[],
constructorIndices (declaration order; empty = no constructor),
fieldDefaults (one DefaultValueDesc per field, RTK_Void = no default),
constructorAccess (one access byte per constructor, parallel to
constructorIndices).

Each class includes: name, fieldCount, superClassIdx, fieldNames[],
fieldTypeKinds[], fieldStructIndices[], fieldClassIndices[],
fieldTypeDescs[], fieldAccess[], methodIndices[], methodAccess[] and
methodVirtual[] (one byte per method, parallel to methodIndices),
constructorIndices (declaration order; empty = no constructor),
constructorAccess (one access byte per constructor, parallel to
constructorIndices). The field segment carries only the fields the
class itself declares — the full instance layout is derived along the
superclass chain at load time, the way a JVM class file or .NET
metadata stores only declared members. Every member function, private
and static included, travels in `methodIndices`; consumers enforce
visibility from the access bytes instead of hiding members.

Each interface includes: name, staticMethodIndices[], a constants
block, and an instance-method signature block — per signature the
method name, formal count, per-formal flags plus a type descriptor, a
return type descriptor, and default values (count == formals).
Instance methods carry no function record (dispatch is by name); the
signatures let a consumer resolve calls through an imported interface
variable and check `implements` contracts.

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
