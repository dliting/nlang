# 21. The `.ncu` Unit Image

A *unit image* is the compiled artifact of one package: `ncc`
writes it, and the loader of every tool reads it back (§3.9.3).
This chapter fixes the image byte layout — the container, the
record tables, the type descriptors, the default values and the
import slots — and the validation a loader performs before an
image runs. §21.1 sets the scope and the format version; §21.2
fixes the container; §21.3–§21.7 define the records; §21.8
states the loading contract.

## 21.1 Scope and Version

**Format version.** This chapter describes format version
**2.16**, recorded in the header as major `2`, minor `16`
(§21.2). A conforming loader accepts exactly this version and
rejects every other: an older image with a "recompile"
diagnostic, a newer image with an "upgrade the toolchain"
diagnostic (§21.8). There is no migration and no partial
acceptance. A format change bumps the version and replaces this
chapter wholesale; the specification describes only the current
version.

**What this chapter fixes.** The container layout (§21.2), the
template table (§21.2.1), the function records (§21.3), the type
records (§21.4), the type descriptors (§21.5), the default values
and default expressions (§21.6), the import slots (§21.7) and the
loading contract (§21.8).

**What it does not fix.** The encoding of the instructions
inside a function's bytecode array: the array is an opaque byte
string here. It is pinned to the same format version — a 2.16
image carries the instruction set of the 2.16 toolchain — but
the opcode values and instruction forms are not part of this
specification. The native calling convention is §23.4; the
archive that packs images is chapter 22.

**Determinism.** Compiling the same source program twice with
the same `ncc` produces byte-identical images: the writer
records no timestamps, and every table follows declaration
order.

## 21.2 Container Layout

**Conventions.** Every integer is written little-endian, least
significant byte first, in two's-complement form. A
*length-prefixed string* is its byte count followed by exactly
that many bytes; there is no terminator, and the image imposes
no text encoding on the bytes. The width tables below use `u8`,
`u16` and `u32` for unsigned integers of 1, 2 and 4 bytes,
`i16` for a signed 2-byte value, and `f32` and `f64` for the
IEEE 754 binary32 and binary64 encodings (§4.2).

**Header.** The image begins with:

| Field | Width | Content |
|---|---|---|
| magic | 8 bytes | `NLANGCU` followed by a zero byte |
| version major | u16 | `2` |
| version minor | u16 | `16` |
| module path | u16 length + bytes | the package path of the unit (§3.9.1) |
| name | u32 length + bytes | the unit name: the same string as the module path in every image the current toolchain writes |
| entry key | u16 length + bytes | the qualified key (§3.6) of the entry function (§9.3); empty for a library image (§3.9.1) |

**Sections.** The header is followed by fourteen sections in a
fixed order. Each section is a u32 entry count followed by that
many records:

| # | Section | Record |
|---|---|---|
| 1 | string constants | u32 length + bytes |
| 2 | template table | §21.2.1 |
| 3 | functions | §21.3 |
| 4 | structs | §21.4 |
| 5 | classes | §21.4 |
| 6 | interfaces | §21.4 |
| 7 | array types | §21.4 |
| 8 | enums | §21.4 |
| 9 | static slots | §21.4 |
| 10 | function imports | §21.7 |
| 11 | class imports | §21.7 |
| 12 | struct imports | §21.7 |
| 13 | enum imports | §21.7 |
| 14 | static-slot imports | §21.7 |

**Names.** Every function, class, struct, interface and
enumeration is recorded under its qualified key
`<package>.<name>`, where `<package>` is the header's module
path (§3.6, §3.9.3). An external artifact is found under a
single-segment package path (§3.9.2), so its images carry
single-segment module paths.

**String constants.** The pool holds the unit's string
literals; the bytecode and the default values of §21.6
reference it by index.

### 21.2.1 The Template Table

The template table records the generic-instantiation descriptors
the unit registers (§4.12.9), one row each:

| Field | Width | Content |
|---|---|---|
| canonical spelling | u16 length + bytes | the instantiation's canonical spelling, e.g. `Box<int>`; empty for a recipe row |
| recipe | u16 length + bytes | the construction recipe behind the spelling; empty for a static row |

An empty recipe means the spelling is the whole fact — the row
describes an instantiation whose arguments are known at compile
time. A non-empty recipe leaves the spelling empty, and the
canonical spelling is composed at runtime from the recipe (forms
containing a type parameter, §4.12.9). The recipe is a tag
stream:

| Tag | Content |
|---|---|
| `0` template head | u16 length + bytes: the template's bare name |
| `1` constant argument | u16 length + bytes: the complete argument spelling |
| `2` receiver argument | u8: the argument index in the receiver's descriptor |
| `3` array wrapper | a u8 rank (§4.10) + one child argument encoding |
| `4` nested instantiation | a u16 length + child recipe bytes (a complete stream led by a template head) |

Rows are self-describing by their leading tag: a row from a
construction site or an instantiation `typeName` leads with the
template head; a row from a non-instantiation operand (such as
`T[]`) is a single argument encoding (tags `1`–`4`). The two
shapes cannot alias, and byte-identical rows still merge.
Linking merges the rows of all units; objects constructed across
units carry their instantiation information as usual (§4.12.8,
§4.12.9).

## 21.3 Function Records

The function section holds one record per function, method and
constructor of the unit, in registration order. Each record:

| Field | Width | Content |
|---|---|---|
| name | u32 length + bytes | the qualified key (§3.6) |
| frame size | u16 | the frame size in bytes, eight bytes per slot: parameters (the receiver included), locals and scratch (§9.2) |
| parameter count | u16 | the formal count, including the implicit receiver slot of a method or constructor (§4.8.7) |
| return kind | u16 | the type-kind code of the return type (§21.5); `0xFE` for `void` |
| intrinsic id | u16 | `0xFFFF` for an ordinary function; otherwise an engine-internal identifier of a built-in intrinsic |
| native flag | u8 | `1` for a body-less `native` declaration (§7.5) |
| static flag | u8 | `1` for a static member (§4.8.8) |
| instance-method flag | u8 | `1` for a non-static member of a class, struct or enum (instance methods and constructors, §4.8.7); its frame carries the slot-0 receiver, and the native call bridge locates the receiver handle through it (§23.5) |
| default count | u16 | followed by that many default value records (§21.6) |
| bytecode size | u32 | followed by that many instruction bytes |
| try block count | u16 | followed by five u16 values per entry |
| local count | u16 | followed by one local descriptor per entry |
| source file | u32 length + bytes | the source path the function was compiled from |
| parameter descriptor count | u16 | followed by one parameter descriptor per entry |
| return descriptor | u16 length + descriptor | the return type descriptor (§21.5); present only when the return kind is not `0xFE` |
| handle mask size | u32 | followed by that many mask bytes |

**Defaults.** The default count is one entry per written
formal: for a free function it equals the parameter count; for
a method or constructor it is one less, because the receiver
carries no default (§7.2).

**Bytecode.** The instruction array is opaque here (§21.1). A
`native` record carries an empty array: the declaration is
body-less by contract (§7.5).

**Try blocks.** Each entry is five 2-byte values: the start and
end bytecode offsets of the protected range, the handler entry
offset, the class-table index of the caught type, and the frame
byte offset of the local that receives the exception (§8.2).

**Local descriptors.** Each entry is the frame byte offset
(u16), the byte size (u16), a parameter flag (u8, `1` for a
formal or the receiver), the declared type's kind code (u8,
§21.5), the bytecode offset at which the local becomes visible
(u16; `0` for entry — parameters and the receiver), and the
name (u32 length + bytes).

**Parameter descriptors.** One per written formal — a method's
receiver is not described: a flags byte (bit `0` set marks an
`out` parameter, §7.2) followed by a length-prefixed type
descriptor (§21.5).

**Handle mask.** The trailing field tells the garbage collector
which frame slots to trace (§9.2). It covers the slots from
the first local — index equal to the parameter count — through
the last frame slot. Each slot carries a 2-bit code: `0` holds
no root, `1` a string handle, `2` a heap index (a class, struct
or array instance), `3` either; four slots share one byte, the
lowest-numbered slot in the two least significant bits. The
size is the band slot count divided by four, rounded up; a
record with an empty band — a `native` or intrinsic record —
writes size `0`.

## 21.4 Type Records

**Access bytes.** A member's declared access is one byte: `0`
default, `1` `private`, `2` `protected`, `3` `public` (§4.8.2).

**Structs.** One record per struct, in declaration order:

| Field | Width | Content |
|---|---|---|
| name | u32 length + bytes | the qualified key (§3.6) |
| field count | u16 | |
| const marker | u8 | `1` for a const struct (§4.8.12), `0` otherwise |
| field names | u32 length + bytes each | declaration order |
| field kinds | u16 each | the field type's kind code (§21.5) |
| field struct indices | u16 each | the struct-table index for a struct-typed field; `0xFFFF` otherwise |
| field class indices | u16 each | the class-table index for a class-typed field; `0xFFFF` otherwise |
| field descriptors | u16 length + descriptor each | §21.5 |
| constructor count | u16 | followed by that many u16 function-table indices, declaration order |
| field default count | u16 | equals the field count; followed by one default value record per field (§21.6) |
| constructor access | u8 each | |

A field default of *no default* (§21.6) leaves the field at
its type's zero value; the others are applied at every
allocation (§4.7.2).

**Classes.** One record per class, in declaration order:

| Field | Width | Content |
|---|---|---|
| name | u32 length + bytes | the qualified key (§3.6) |
| field count | u16 | the fields this class declares |
| super index | i16 | the class-table index of the superclass; `-1` for none |
| const marker | u8 | `1` for a const class (§4.8.12), `0` otherwise |
| field names | u32 length + bytes each | |
| field kinds | u16 each | |
| field struct indices | u16 each | |
| field class indices | u16 each | |
| field descriptors | u16 length + descriptor each | |
| field access | u8 each | |
| method count | u16 | followed by that many u16 function-table indices: every member this class declares, static and instance, declaration order |
| method access | u8 each | |
| method virtual flags | u8 each | `1` for a `virtual` method (§4.8.9) |
| constructor count | u16 | followed by that many u16 function-table indices |
| constructor access | u8 each | |
| static field count | u16 | each entry: a u32-length name, a length-prefixed type descriptor, an access byte, a const flag (`1` = `const`), and a u16 slot index into the static slot table — `0xFFFF` marks a compile-time-folded constant, which carries its folded value as a default value record (§21.6) instead |
| static-initializer index | u16 | the function-table index of the class's static initializer (§4.8.8); `0xFFFF` = none |

The field list carries only the fields this class declares:
the full instance layout is derived at load time by walking the
superclass chain base-first (§4.8.5). Constructors also appear
in the method list; the constructor table selects them.

**Interfaces.** One record per interface, in declaration order:

| Field | Width | Content |
|---|---|---|
| name | u16 length + bytes | the qualified key (§3.6) |
| static method count | u16 | followed by that many u16 function-table indices |
| constant count | u16 | each entry: a u16-length name, a length-prefixed type descriptor, a default value record (§21.6), and a u16 slot index into the static slot table — `0xFFFF` marks a compile-time-folded constant (the value record carries it), anything else is a runtime-initialized constant (§4.9.3) |
| signature count | u16 | each entry: a u16-length name, a u16 parameter count, per formal a flags byte and a length-prefixed type descriptor, a length-prefixed return descriptor, and a u16 default count followed by that many default value records |
| static-initializer index | u16 | the function-table index of the interface's static initializer; `0xFFFF` = none |

Interface constants are scalar or string (§4.9.3); their default
value record carries the folded initializer. Instance-method
signatures let a consumer resolve calls on an imported
interface-typed receiver and check the `implements` contract
across the package border (§4.9.1). The return descriptor is
always present: `void` is the `0xFE` kind byte. Interface
members are implicitly public and non-virtual, so no access or
virtual bytes are recorded.

**Array types.** One record per distinct array type the unit
uses: the element kind (u8, §21.5), the element type index
(u16; `0xFFFF` for a primitive element), and the rank (u8,
§4.10).

**Enums.** The enum section is three tables in parallel, one
row per enumeration, in declaration order. First the member
values: a u32 count followed by each value name (u32 length +
bytes). Then the qualified keys: one per enumeration (u16
length + bytes), no separate count. Then the member methods: a
u32 count followed by, per method, a u16 function-table index
and an access byte (§4.6.2).

**Static slots.** One record per runtime-addressed static the
unit declares — mutable class statics, runtime-initialized
interface constants, and static locals (§4.8.8);
compile-time-folded constants occupy no slot. Bytecode
addresses a slot by its index into this table:

| Field | Width | Content |
|---|---|---|
| name | u32 length + bytes | the member name |
| owner key | u32 length + bytes | the qualified key of the owning class or interface |
| owner kind | u8 | `0` class owner, `1` interface owner |
| runtime-const flag | u8 | `1` for a runtime-initialized constant |
| initializer flag | u8 | `1` when a static-initializer function assigns the slot |
| type | u16 length + descriptor | §21.5 |

## 21.5 Type Descriptors

A *type descriptor* is the recursive encoding of one declared
type. Descriptors appear as field descriptors, parameter
descriptors, return descriptors, interface constant types and
interface instance-method signatures (§21.3, §21.4). On the
wire a descriptor is a u16 byte count followed by that many
descriptor bytes; a count of `0` records
a type not expressible in this grammar, and a consumer
rebuilding a stub for such a member degrades to an `int`
placeholder.

The descriptor bytes are a kind code followed by a payload:

| Kind | Type | Payload |
|---|---|---|
| `0` | `int` | none |
| `1` | `float` | none |
| `2` | `string` | none |
| `3` | a struct | u16 struct-table index |
| `4` | a class | u16 class-table index |
| `5` | an array | one nested element descriptor, then a rank byte (§4.10) |
| `6` | a boxed primitive | none (§4.11) |
| `7` | a function value | a parameter count byte, then count+1 nested descriptors: the parameters, then the return type (`0xFE` for `void`) (§7.4) |
| `8` | `List` | one nested element descriptor (§4.12) |
| `9` | `Dict` | two nested descriptors: the key type, then the value type (§4.12) |
| `10`–`19` | `byte` `ubyte` `short` `ushort` `uint` `long` `ulong` `double` `bool` `char` | none |
| `0xFB` | not serializable | none |
| `0xFE` | `void` | none |

**Ceilings.** A descriptor nests at most 8 levels deep and
spans at most 4096 bytes; a parameter descriptor list carries
at most 256 entries. The writer records a container whose
descriptor would exceed the nesting or size ceiling as *not
serializable*; a reader rejects a descriptor that exceeds
either.

## 21.6 Default Values and Default Expressions

A *default value record* is a fixed-layout record: every
channel is always present, and the tag selects which one is
meaningful. It appears as a formal's default (§21.3), a struct
field's default (§21.4) and an interface constant's value
(§21.4).

| Field | Width |
|---|---|
| tag | u8 |
| int channel | u32 |
| float channel | f32 |
| string index | u32 |
| long channel | i64 |
| double channel | f64 |
| expression length | u16, followed by that many expression bytes |

| Tag | Meaning |
|---|---|
| `0xFE` | no default |
| `0xFD` | the `null` literal, for any reference type |
| `0` | an `int` in the int channel; also `bool` (`0` or `1`) and `char` (the code point) |
| `1` | a `float` in the float channel |
| `2` | a `string`: the string index names the producer's string-constant pool (§21.2); the import merge remaps it into the consumer's pool (§21.7) |
| `15` / `16` | a `long` or `ulong` in the long channel (the `ulong` bit pattern) |
| `17` | a `double` in the double channel |
| `0xFB` | a default expression: the expression bytes hold the tree below |
| `0xFC` | a default outside the expressible subset: a consumer that must materialize it reports an error |

Struct field defaults and interface constants carry only
foldable tags — never `0xFB` or `0xFC` (§4.7.2, §4.9.3).

**Default expressions.** A default outside the foldable subset
rides a small expression tree; each node begins with a tag
byte:

| Tag | Content |
|---|---|
| `0` `1` `15` `16` `17` | a scalar literal in the matching channel width: 4 bytes for `int` and `float`, 8 for `long`, `ulong` and `double` |
| `2` | a string: a u32 string-constant index (producer pool) |
| `0xFD` | `null` |
| `0x20` | a reference to an earlier formal of the same function: a u8 formal index (§7.2) |
| `0x21` | unary negation, then one child tree |
| `0x22` | a binary expression: an operator byte (`0` `+`, `1` `-`, `2` `*`, `3` `/`, `4` `%`, `5` `<<`, `6` `>>`), then two child trees |
| `0x23` | a call: a u16 length and the callee's qualified key, a u32 parameter count, a u8 argument count, then that many child trees |

A default expression tree nests at most 16 levels and spans at
most 4096 bytes; the writer records a default whose expression
would exceed either ceiling as `0xFC`, and a reader rejects a
tree that exceeds either.

## 21.7 Import Slots

A reference to a declaration of another package is recorded as
an *import slot* appended to the table it belongs to: the
table's own entries occupy indices `0..n-1` and its imports
occupy `n..n+m`, so a compile-time reference to an external
declaration is a table index above the own-entry count. The
five import sections follow the table order of §21.2.

Every entry:

| Field | Width | Content |
|---|---|---|
| module path | u16 length + bytes | the target package path (§3.9.1) |
| name | u16 length + bytes | the qualified key (§3.6); a bare name when the owner key is set |
| parameter count | u32 | the target's formal count; meaningful for function slots |

The function section adds:

| Field | Width | Content |
|---|---|---|
| owner key | u16 length + bytes | empty for a package-level function; otherwise the qualified key of the owning class or struct |
| static flag | u8 | part of the linkage identity: a static and an instance member of the same name and arity are distinct slots |
| owner kind | u8 | `0` package-level, `1` class member, `2` struct member |
| owner ordinal | u16 | the member's position in the owner's table: the constructor-table index for a constructor, otherwise the method-table position with constructor entries skipped; `0xFFFF` for a package-level function |

Overloaded members share a name and a parameter count, so the
owner ordinal single-selects the target. The loader binds each
slot by loading the target package and locating the member
(§3.9.3).

The static-slot import section (§21.2) uses the function
section's record shape with its own owner-kind domain: `0`
class owner, `1` interface owner — an external static slot
reference names the owning type, never a member position.

## 21.8 Loading and Validation

A loader reads the image strictly and rejects it whole: a
rejected image never runs in part. The rejection sites:

- **magic.** Header bytes other than the magic → reject.
- **truncation.** A stream that ends mid-record → reject.
- **version.** Below 2.16 → reject with a "recompile with the
  current compiler" diagnostic; above 2.16 → reject with an
  "upgrade the toolchain" diagnostic (§21.1).
- **count and length caps.** Counts and lengths are bounded
  before allocation. Capped at 2^24: the unit name, the string
  constant count and each constant length, the array type
  count, the function, struct and class counts and their name
  and field-name lengths, the enumeration count, the enum
  value count and each value name length, and each import
  section count. Capped at 2^20: the interface count and the
  enum member-method count. Capped at 2^16: local names,
  source-file paths and handle-mask bytes. A larger value is
  rejected before allocation.
- **descriptor indices.** After the type tables are read, every
  struct and class index carried by a type descriptor — in
  function parameter and return descriptors, field descriptors,
  interface constant types and interface instance-method
  signatures — must address an existing table entry.
- **owner kind.** An import slot with an owner kind above `2` →
  reject.
- **import tails.** An import count larger than its table's own
  entry count → reject.
- **handle mask.** A mask byte count other than the band size
  of §21.3 — the frame slot count (the frame size divided by
  eight) minus the parameter count, one code per four slots
  rounded up; zero for an empty band — → reject.
- **entry key.** An empty key means no entry (a library image);
  a non-empty key must name a function of the image, or the
  image is rejected.

A rejected image surfaces as a load-time error of the loading
tool (§3.9.3); the language defines no recovery.
