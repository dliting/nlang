# Standard Library Intrinsics (Phase 11)


Namespace-qualified calls (`math.sin(x)`, `io.print(s)`, `fs.join(a,b)`) and
the built-in string methods compile to `OP_CallIntrinsic` — zero
`CompiledFunction` records, zero `.nmod` function entries. The compiler-side
truth is the single table pair in `include/nlang/vm/StdLib.h`
(`kStdLibTable` + `kStringMethodTable`): the resolver intercepts qualified
and member calls against it, `VmBackend::EmitStdlibCall` emits the call.
Intrinsic ids are module-local — `RemapBytecode` never touches them — so
cross-module imports have no id problems.

**Argument ABI** — two shapes, deliberately different:

- namespace free functions read arguments from `callParamBase` slot 0
  upward, **no `this` pointer** (unlike every other intrinsic family, where
  `this` occupies slot 0);
- string methods use the `string.equals` ABI: receiver pool idx at
  `callParamBase[0]`, args from slot 1.

**VM dispatch chain**: `ExecuteIntrinsic` (VmExecutor.cpp) delegates to one
member function per family TU; each returns `false` when the id is not its
own and the chain falls through — math → io → string → fs → unknown-id
throw. Each family is independently extensible in its own TU.

**Intrinsic id allocation** (CompiledModule.h). Blocks are contiguous and
statically bound to the StdLib.h tables on both sides (every entry's id lies
inside its block, and entry count == block size — a mismatch is a compile
error, not a runtime "unknown intrinsic" hole):

| Ids | Prefix | Family |
|-----|--------|--------|
| 0-14 | INTR_BS_* | ByteStream |
| 20-33 | INTR_FS_* | FileStream (why the filesystem block below uses `FileSystem_`) |
| 40-43, 61-63 | INTR_Object_/String_/List_/Dict_ | protocol methods + toString |
| 44-52 | INTR_List_* | List\<T\> methods |
| 53-60 | INTR_Dict_* | Dict\<K,V\> methods |
| 64-69 | INTR_*Exception_Ctor | exception ctors, incl. IOException (Step 2) |
| 70-94 | INTR_Math_* | math, 25 functions |
| 95-106 | INTR_String_* | string methods, 12 new (Equals/GetHashCode stay at 42/43) |
| 110-114 | INTR_Io_* | io, 5 functions |
| 120-127 | INTR_FileSystem_* | fs, 8 functions |

**New opcodes** (Step 3b): `OP_Less_str` / `OP_LessEqual_str` /
`OP_Greater_str` / `OP_GreaterEqual_str` — bytewise relational comparison
(UTF-8 byte order == code point order), mirroring `OP_Eq_str`. The variant
dispatch keys on the LEFT operand's `EvalDataType`, same as every binary
opcode.

**Positional-array guard**: `s_OpCodeNames` (OpCodeTable.cpp) is a positional
array — a missing row is not a compile error but an out-of-bounds read at
runtime. `static_assert(std::size(s_OpCodeNames) == +OpCode::OP_Count)`
binds the size. A new opcode is still 6 manual touch points: enum, names,
`InstructionStride` (default `assert(false)` — a miss corrupts cross-module
remap in Release), codegen, executor, ndisasm.
