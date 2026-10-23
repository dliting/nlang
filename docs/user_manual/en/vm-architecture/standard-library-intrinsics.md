# Standard Library Intrinsics

Intrinsics are operations the VM implements directly in C++: they have no
NLang function body. Only the string-method family is called through
`OP_CallIntrinsic` with no entry in the module's function table; every
other family — byte and file streams, `List<T>`/`Dict<K,V>`, the Object
protocol, exception constructors and the concurrency operations — rides a
stub `CompiledFunction` record stamped with its intrinsic ID and is called
through the ordinary method- or function-call opcodes. The standard
library splits across these mechanisms: `math`, `io` and `fs` are ordinary
library sources whose native members run from `nlang_<package>.dll`,
`thread`'s native declarations are implemented inside the VM as intrinsic
stubs, and `concurrent` is pure NLang — all described in
[the library mechanism](library-mechanism.md). A call such as
`math.sin(x)` compiles to `OP_CallFunc` against the inlined library
declaration, so it carries a `CompiledFunction` record like any user
function.

**What reaches `OP_CallIntrinsic`**: the string-method family
(`s.substring(1)`, `s.split(d)`, …), resolved through `kStringMethodTable`
in `include/nlang/vm/StdLib.h` — the resolver intercepts the member call
against that table and `src/vm/backend/EmitExprMemberString.cpp` emits the
call. An intrinsic's identifier (ID) is module-local — the linker's operand
remap (`NcuRemapBytecodeOperands`, `src/vm/NcuLinkerRemap.cpp`) never
rewrites it — so cross-module imports have no ID problems.

**Argument application binary interface (ABI)**: the receiver occupies `callParamBase[0]` and arguments
start at slot 1, the `string.equals` shape. `OP_CallIntrinsic` carries no
argument count, so a call shorter than a table entry allows stages the
missing trailing value synthetically (`StringTrailingDefault`).

**VM dispatch chain**: `ExecuteIntrinsic` (VmExecutorIntrinsics.cpp)
delegates to one member function per family translation unit (TU); each returns `false` when
the ID is not its own and the chain falls through — ByteStream →
FileStream → the thread-family arm (`IntrinsicsThread.cpp`, IDs 129-143)
→ inline Object/List/Dict-protocol and exception-ctor arms → string →
unknown-ID throw. Each family is independently extensible in its own TU.

**Intrinsic ID allocation** (`include/nlang/vm/Intrinsics.h`). Blocks are
contiguous. The string-method block is statically bound to the
`kStringMethodTable` on both sides — the block bounds live in
`Intrinsics.h`, the per-entry ID check and the entry-count check in
`StdLib.h`, and a mismatch is a compile error, not a runtime "unknown
intrinsic" hole; the other families have no source table to bind against:

| IDs | Prefix | Family |
|-----|--------|--------|
| 0-18 | INTR_BS_* | ByteStream |
| 20-37 | INTR_FS_* | FileStream (file streams, not the `fs` library) |
| 40-43, 61-63 | INTR_Object_/String_/List_/Dict_ | protocol methods + toString |
| 44-52 | INTR_List_* | List\<T\> methods |
| 53-60 | INTR_Dict_* | Dict\<K,V\> methods |
| 64-69 | INTR_*Exception_Ctor | exception ctors, incl. IOException |
| 95-112 | INTR_String_* | string methods, 18 of them (Equals/GetHashCode live at 42/43) |
| 128, 144 | INTR_InvalidCastException_Ctor / INTR_ChannelClosedException_Ctor | exception ctors allocated after the string block |
| 129-133 | INTR_Thread_* | `thread` namespace free functions |
| 134-141 | INTR_Mutex_/INTR_Channel_ | Mutex/Channel methods |
| 142-143 | INTR_Future_* | Future methods |

Allocation is append-only: free IDs 19 and 38-39 are reserved; the
70-94 and 113-127 blocks are retired and will not be reissued.

**Relational string opcodes**: `OP_Less_str` / `OP_LessEqual_str` /
`OP_Greater_str` / `OP_GreaterEqual_str` — bytewise relational comparison
(Unicode Transformation Format (UTF-8) byte order == code point order), mirroring `OP_Eq_str`. The variant
dispatch keys on the LEFT operand's `EvalDataType`, same as every binary
opcode.

**Positional-array guard**: `s_OpCodeNames` (OpCodeTable.cpp) is a positional
array — a missing row is not a compile error but an out-of-bounds read at
runtime. `static_assert(std::size(s_OpCodeNames) ==
static_cast<size_t>(OpCode::OP_Count))` binds the size. A new opcode is 6
manual touch points: the enum, the name table, the remap stride
`NcuInstructionStride` (NcuLinkerRemap.cpp; default `assert(false)` — a
miss corrupts cross-module remap in Release), codegen, the executor, and
ndisasm (its own `InstructionStride` walk plus the printing switch).
