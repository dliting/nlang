#pragma once
#include "TypeDesc.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace nlang {

//.nmod format version. Single source of truth shared by the writer
//(WriteCompiledModule in ModuleSaver.cpp) and the reader (ModuleLoader) —
//bump both sides atomically by editing only these constants.
inline constexpr uint16_t NMOD_FORMAT_MAJOR = 1;
//v1.7 (Phase 11): stdlib namespace intrinsics + reserved namespaces. No
//field-layout change, but the later relational string opcodes share this
//version step, so older VMs must refuse these modules outright.
//v1.8 (Phase 13): first-class function values — RTK_Func heap records,
//the OP_MakeFunc/OP_CallDelegate/OP_Eq_func/OP_Ne_func/OP_Func_to_str
//opcodes (Step 1: free function references) plus OP_MakeBoundFunc/
//OP_MakeVFunc/OP_CallDelegateOut (Step 2: bound method references).
//Older VMs cannot execute these opcodes; refuse outright.
//v1.9 (debugger): per-function source file path — CompiledFunction::
//sourceFile, emitted unconditionally after the locals block. Cross-module
//breakpoint addressing (b file.n:LINE) depends on it. No new opcodes.
//v1.10 (array redesign): SEMANTIC floor — no new serialized fields, but
//array struct/class fields now store RTK_Array as their fieldTypeKinds
//entry (previously the element kind, e.g. RTK_Int32). A v1.9 module from
//an older ncc carries the old field-kind meaning and would misroute GC
//marking and struct serialization dispatch; the loader refuses it.
//v1.11 (C-period generic array args): SEMANTIC floor — no new serialized
//fields, but array-typed elements of generic containers (List<T[]> /
//Dict keys and values) now flow as raw array handles with no boxing, and
//foreach loop variables over them occupy RTK_Array local slots the GC
//traces. A v1.10 module from an older ncc boxes those elements into
//primitive slots the GC never traces; the loader refuses it.
//v1.12 (type descriptors): true formal / return / field types — recursive
//TypeDesc payloads (see TypeDesc.h) after the source-file block (per
//formal, count-prefixed; return, gated on returnTypeKind != RTK_Void)
//and per struct/class field. Consumers rebuild imported stubs with real
//signatures instead of return-kind placeholders, so call-site type
//checking for imported callees is now performed; the loader refuses
//v1.11 and older outright.
//v1.13 (basic types): ten new scalar RTK kinds (RTK_Byte..RTK_Char,
//10..19; 8/9 stay the TypeDesc descriptor-only kinds), generalized
//numeric opcodes with kind immediates, bool conditions, 8-byte scalar
//slots (long/ulong/double) and the unified 3-slot boxed record. Older
//modules are refused outright.
//v1.14 (debugger): per-local declaration PC in the locals block — the
//debug views filter locals whose declaration the paused PC has not
//reached yet (scope visibility). A v1.13 module lacks the two bytes per
//local and misparses every name that follows.
inline constexpr uint16_t NMOD_FORMAT_MINOR = 14;

//Runtime type kind constants for serialization.
//Compile-time NK_* values exceed uint8_t range, so we map them.
//The scalar codes (RTK_Int32/RTK_Float = 0/1 plus RTK_Byte..RTK_Char
//= 10..19) live in nlang/runtime/PrimitiveTypes.h — the single source
//shared by compiler and VM.
static constexpr uint8_t RTK_String = 2;
static constexpr uint8_t RTK_Struct = 3;
static constexpr uint8_t RTK_Class  = 4;
static constexpr uint8_t RTK_Array  = 5;
static constexpr uint8_t RTK_Boxed  = 6;  //boxed primitive (slot[0]=tag, slots[1..2]=value; 8-byte family uses both cells)
static constexpr uint8_t RTK_Func   = 7;  //Phase 13: function handle (slot[0]=target, slot[1]=this, slot[2]=form; 0=static, 1=virtual)
//Descriptor-only kinds RTK_List / RTK_Dict / RTK_NonSerialized live in
//TypeDesc.h (they appear inside TypeDesc, never in these legacy bytes).
static constexpr uint8_t RTK_Null   = 0xFD;  //Option B: null default value (any reference type)
static constexpr uint8_t RTK_Unfoldable = 0xFC;  //Option B: had default but not constant-foldable
static constexpr uint8_t RTK_Void   = 0xFE;  //used for ctor/void method stubs

//--- Slot-width ABI (0.7.5) ----------------------------------------------
//The frame and the heap use two different stride contracts; both live
//here because they are shared by the backend (producer) and the
//executor (consumer) — a per-TU constant on either side would drift.
//
//Frame: every slot — params, locals, temps, return, call staging, eval
//area — is one uniform 8-byte cell. 4-byte kinds live value-extended in
//the low half (narrow ints sign/zero-extended by their row); the upper
//half may hold garbage that no kind-correct consumer ever reads.
//long/ulong/double use all 8 bytes. One uniform stride replaces the
//former 4-byte per-TU VALUE_SIZE statics, so no emission site has to
//pick a copy width by kind (returns, switch value slots, temps and
//eval-area claims are width-correct by construction).
static constexpr uint16_t kFrameSlotBytes = 8;

//Heap: records are vectors of int32 cells. Struct/class data fields are
//uniform 2-cell (8-byte) slots — field i sits at cell 2i (struct) /
//1+2i (class, cell 0 = classIdx for virtual dispatch) — which keeps the
//positional field walks kind-free. Array elements are registry-driven:
//1 cell for kinds up to 4 bytes, 2 cells for the long/double family.
static constexpr uint16_t kHeapCellBytes = 4;
static constexpr uint16_t kHeapFieldStrideBytes = 8;  // = 2 * kHeapCellBytes

//Cells occupied by one array element of the given RTK elem kind:
//2 for the 8-byte scalar family (long/ulong, double when it lands),
//1 for everything else (≤4-byte scalars and every reference kind).
//Element i of an array record lives at cell 3 + i*cells.
inline int ArrayElemCells(uint8_t elemRtk)
{
    int pi = ScalarPrimIndexOfRtk(elemRtk);
    return (pi >= 0 && kScalarPrims[pi].slotWidth > 4) ? 2 : 1;
}

struct LocalDescriptor {
    uint16_t offset = 0;
    uint16_t size = 0;
    uint8_t  isParam = 0;
    uint8_t  typeKind = 0;
    //Debugger scope visibility (v1.14): bytecode offset where the local
    //joins its function's flat frame. 0 = live from entry (params and
    //the implicit this receiver). The debug views hide a local while the
    //paused statement PC (frame.currentPc) is below this value; the GC
    //root scan deliberately ignores it — slots are zero-initialized at
    //every call, so tracing the whole table from entry is safe.
    uint16_t declPc = 0;
    std::string name;
};

struct CompiledStruct {
    std::string name;
    uint16_t fieldCount = 0;
    std::vector<std::string> fieldNames;
    std::vector<uint16_t> fieldTypeKinds;       // RTK_* per field
    std::vector<uint16_t> fieldStructIndices;    // struct index for struct-typed fields, 0xFFFF for non-struct
    std::vector<uint16_t> fieldClassIndices;     // class index for class-typed fields, 0xFFFF for non-class
    //v1.12: per-field recursive type descriptors (empty entries = not
    //expressible). Survive the import merge (indices remapped). Wire
    //fidelity only — stubs keep their members empty, so nothing reads
    //these back for reconstruction; they exist so a consumer re-saving
    //the module emits correct descriptors.
    std::vector<TypeDesc> fieldTypeDescs;
};

struct CompiledArrayType {
    uint8_t  elemKind = 0;       // RTK_Int32/RTK_Float/RTK_String/RTK_Struct/RTK_Class
    uint16_t elemTypeIdx = 0xFFFF; // struct/class index (0xFFFF for primitives)
};

//Builtin intrinsic function IDs.
//Used by CompiledFunction::intrinsicId to dispatch VM-side methods.
static constexpr uint16_t INTR_None = 0xFFFF;

//ByteStream intrinsics.
static constexpr uint16_t INTR_BS_Ctor         = 0;
static constexpr uint16_t INTR_BS_WriteInt     = 1;
static constexpr uint16_t INTR_BS_ReadInt      = 2;
static constexpr uint16_t INTR_BS_WriteFloat   = 3;
static constexpr uint16_t INTR_BS_ReadFloat    = 4;
static constexpr uint16_t INTR_BS_WriteString  = 5;
static constexpr uint16_t INTR_BS_ReadString   = 6;
static constexpr uint16_t INTR_BS_Length       = 7;
static constexpr uint16_t INTR_BS_Position     = 8;
static constexpr uint16_t INTR_BS_Reset        = 9;
static constexpr uint16_t INTR_BS_Close        = 10;
static constexpr uint16_t INTR_BS_WriteStruct = 11;
static constexpr uint16_t INTR_BS_ReadStruct  = 12;
static constexpr uint16_t INTR_BS_WriteObject = 13;
static constexpr uint16_t INTR_BS_ReadObject  = 14;
//0.7.5 Task 9: 64-bit scalar family (8-byte wire form, mirroring the
//4-byte writeInt/readInt/writeFloat/readFloat quartet).
static constexpr uint16_t INTR_BS_WriteLong   = 15;
static constexpr uint16_t INTR_BS_ReadLong    = 16;
static constexpr uint16_t INTR_BS_WriteDouble = 17;
static constexpr uint16_t INTR_BS_ReadDouble  = 18;

//FileStream intrinsics.
static constexpr uint16_t INTR_FS_Ctor         = 20;
static constexpr uint16_t INTR_FS_WriteInt     = 21;
static constexpr uint16_t INTR_FS_ReadInt      = 22;
static constexpr uint16_t INTR_FS_WriteFloat   = 23;
static constexpr uint16_t INTR_FS_ReadFloat    = 24;
static constexpr uint16_t INTR_FS_WriteString  = 25;
static constexpr uint16_t INTR_FS_ReadString   = 26;
static constexpr uint16_t INTR_FS_Length       = 27;
static constexpr uint16_t INTR_FS_Position     = 28;
static constexpr uint16_t INTR_FS_Close        = 29;
static constexpr uint16_t INTR_FS_WriteStruct = 30;
static constexpr uint16_t INTR_FS_ReadStruct  = 31;
static constexpr uint16_t INTR_FS_WriteObject = 32;
static constexpr uint16_t INTR_FS_ReadObject  = 33;
//0.7.5 Task 9: 64-bit scalar family (34..37; 30..33 were already taken
//by the struct/object serialization block above).
static constexpr uint16_t INTR_FS_WriteLong   = 34;
static constexpr uint16_t INTR_FS_ReadLong    = 35;
static constexpr uint16_t INTR_FS_WriteDouble = 36;
static constexpr uint16_t INTR_FS_ReadDouble  = 37;

//Object/String intrinsic methods (Phase 8e-1).
static constexpr uint16_t INTR_Object_Equals        = 40;
static constexpr uint16_t INTR_Object_GetHashCode   = 41;
static constexpr uint16_t INTR_String_Equals        = 42;
static constexpr uint16_t INTR_String_GetHashCode   = 43;
//Phase 8e-9b: Object.toString() default intrinsic. ID is 61 because List/Dict
//intrinsics (44-60) were allocated before this phase. Registration order in
//VmBackend remains "紧跟 GetHashCode" (semantic adjacency), only the numeric
//ID is non-contiguous with the 8e-1 Object block.
static constexpr uint16_t INTR_Object_toString      = 61;

//List<T> intrinsic methods (Phase 8e-3, erasure-style — all elements stored as heap idxs).
static constexpr uint16_t INTR_List_Ctor        = 44;
static constexpr uint16_t INTR_List_Add         = 45;
static constexpr uint16_t INTR_List_Get         = 46;
static constexpr uint16_t INTR_List_Set         = 47;
static constexpr uint16_t INTR_List_Length      = 48;
static constexpr uint16_t INTR_List_RemoveAt    = 49;
static constexpr uint16_t INTR_List_IndexOf     = 50;
static constexpr uint16_t INTR_List_Contains    = 51;
static constexpr uint16_t INTR_List_Clear       = 52;

//Dict<K,V> intrinsic methods (Phase 8e-4, erasure-style — keys and values
//stored as heap idxs in a side table, linear-scan lookup).
static constexpr uint16_t INTR_Dict_Ctor        = 53;
static constexpr uint16_t INTR_Dict_Set         = 54;
static constexpr uint16_t INTR_Dict_Get         = 55;
static constexpr uint16_t INTR_Dict_ContainsKey = 56;
static constexpr uint16_t INTR_Dict_Remove      = 57;
static constexpr uint16_t INTR_Dict_Clear       = 58;
static constexpr uint16_t INTR_Dict_Count       = 59;
//Phase 8e-5: Dict.Keys() — returns a new List<K> populated from dict entries' keys.
static constexpr uint16_t INTR_Dict_Keys        = 60;
//Phase 9b-pre: collection toString (Python-style "[a, b, c]" / "{k: v}").
static constexpr uint16_t INTR_List_toString    = 62;
static constexpr uint16_t INTR_Dict_toString    = 63;

//Phase 9d: Exception class ctor intrinsics. All 5 Exception-family ctors
//share the same intrinsic dispatch (single ExecuteIntrinsic case handles
//all 5 IDs); the distinct IDs exist so that user `new MyException("msg")`
//resolves to the correct ctor stub for type-checking. Subclass ctors set
//message + allocate an empty List<string> for backtrace, identical to base.
static constexpr uint16_t INTR_Exception_Ctor                = 64;
static constexpr uint16_t INTR_NullPointerException_Ctor     = 65;
static constexpr uint16_t INTR_DivByZeroException_Ctor       = 66;
static constexpr uint16_t INTR_IndexOutOfBoundsException_Ctor = 67;
static constexpr uint16_t INTR_AssertionException_Ctor       = 68;
static constexpr uint16_t INTR_IOException_Ctor              = 69;

//Phase 11: stdlib namespace intrinsics. Allocation map (see StdLib.h for
//the shared function table; args from callParamBase slot 0, no this):
//  69          IOException ctor
//  70-94       math, 25 functions
//  95-112      string methods, 18 (0.7.5 Task 8 extended the block by the
//              six char-bridge methods, which pushed io out of 110-114 —
//              io moved to 115-119; no compat: ids are .nmod immediates
//              and every module recompiles; existing Equals/GetHashCode
//              keep their 8e-1 ids 42/43 — only the implementation moved)
//  113-119     io, 7 (0.7.7 write/eprint backfilled the slots the
//              0.7.5 char-bridge move left free)
//  120-127     fs, 8 (prefix INTR_FileSystem_*: INTR_FS_* is FileStream)
static constexpr uint16_t INTR_Math_Sqrt   = 70;
static constexpr uint16_t INTR_Math_Sin    = 71;
static constexpr uint16_t INTR_Math_Cos    = 72;
static constexpr uint16_t INTR_Math_Tan    = 73;
static constexpr uint16_t INTR_Math_Asin   = 74;
static constexpr uint16_t INTR_Math_Acos   = 75;
static constexpr uint16_t INTR_Math_Atan   = 76;
static constexpr uint16_t INTR_Math_Atan2  = 77;
static constexpr uint16_t INTR_Math_Pow    = 78;
static constexpr uint16_t INTR_Math_Exp    = 79;
static constexpr uint16_t INTR_Math_Log    = 80;
static constexpr uint16_t INTR_Math_Absi   = 81;
static constexpr uint16_t INTR_Math_Absf   = 82;
static constexpr uint16_t INTR_Math_Mini   = 83;
static constexpr uint16_t INTR_Math_Maxi   = 84;
static constexpr uint16_t INTR_Math_Minf   = 85;
static constexpr uint16_t INTR_Math_Maxf   = 86;
static constexpr uint16_t INTR_Math_Clampi = 87;
static constexpr uint16_t INTR_Math_Clampf = 88;
static constexpr uint16_t INTR_Math_Floor  = 89;
static constexpr uint16_t INTR_Math_Ceil   = 90;
static constexpr uint16_t INTR_Math_Round  = 91;
static constexpr uint16_t INTR_Math_Random = 92;
static constexpr uint16_t INTR_Math_Srand  = 93;
static constexpr uint16_t INTR_Math_Randomi = 94;
//Executable allocation invariant: the math block is contiguous 70-94 and
//kStdLibTable references exactly this range (an unimplemented id is only
//a runtime "unknown intrinsic" hole, so bind the block size here).
static constexpr uint16_t kMathIntrinsicFirst = 70;
static constexpr uint16_t kMathIntrinsicCount = 25;
static_assert(INTR_Math_Sqrt == kMathIntrinsicFirst,
    "math intrinsic block must start at kMathIntrinsicFirst");
static_assert(INTR_Math_Randomi
        == kMathIntrinsicFirst + kMathIntrinsicCount - 1,
    "math intrinsic block must be contiguous up to Randomi");

//Phase 11 Step 2: io namespace intrinsics (content IO — console + text
//files; see StdLib.h). Same executable-contiguity invariant as math.
//write/eprint (0.7.7) take the 113/114 slots left free when the block
//moved to 115 for the 0.7.5 char bridge — no existing id changes.
static constexpr uint16_t INTR_Io_Write      = 113;
static constexpr uint16_t INTR_Io_EPrint     = 114;
static constexpr uint16_t INTR_Io_Print      = 115;
static constexpr uint16_t INTR_Io_ReadLine   = 116;
static constexpr uint16_t INTR_Io_ReadFile   = 117;
static constexpr uint16_t INTR_Io_WriteFile  = 118;
static constexpr uint16_t INTR_Io_AppendFile = 119;
static constexpr uint16_t kIoIntrinsicFirst = 113;
static constexpr uint16_t kIoIntrinsicCount = 7;
static_assert(INTR_Io_Write == kIoIntrinsicFirst,
    "io intrinsic block must start at kIoIntrinsicFirst");
static_assert(INTR_Io_AppendFile
        == kIoIntrinsicFirst + kIoIntrinsicCount - 1,
    "io intrinsic block must be contiguous up to AppendFile");

//Phase 11 Step 3: built-in string methods (receiver-dispatched via
//OP_CallIntrinsic — receiver string idx at callParamBase[0], args from
//slot 1). Equals/GetHashCode keep their 8e-1 ids 42/43; only the
//implementation moves to IntrinsicsString.cpp.
static constexpr uint16_t INTR_String_Substring  = 95;
static constexpr uint16_t INTR_String_IndexOf    = 96;
static constexpr uint16_t INTR_String_StartsWith = 97;
static constexpr uint16_t INTR_String_EndsWith   = 98;
static constexpr uint16_t INTR_String_Contains   = 99;
static constexpr uint16_t INTR_String_ToUpper    = 100;
static constexpr uint16_t INTR_String_ToLower    = 101;
static constexpr uint16_t INTR_String_Trim       = 102;
static constexpr uint16_t INTR_String_Split      = 103;
static constexpr uint16_t INTR_String_Replace    = 104;
static constexpr uint16_t INTR_String_ToInt      = 105;
static constexpr uint16_t INTR_String_ToFloat    = 106;
//0.7.5 char bridge (Task 8): the six code-point / strict-parse methods.
static constexpr uint16_t INTR_String_CharAt     = 107;
static constexpr uint16_t INTR_String_CharCount  = 108;
static constexpr uint16_t INTR_String_ToChar     = 109;
static constexpr uint16_t INTR_String_ToLong     = 110;
static constexpr uint16_t INTR_String_ToDouble   = 111;
static constexpr uint16_t INTR_String_ToBool     = 112;
static constexpr uint16_t kStringMethodIntrinsicFirst = 95;
static constexpr uint16_t kStringMethodIntrinsicCount = 18;
static_assert(INTR_String_Substring == kStringMethodIntrinsicFirst,
    "string-method intrinsic block must start at its First constant");
static_assert(INTR_String_ToBool
        == kStringMethodIntrinsicFirst + kStringMethodIntrinsicCount - 1,
    "string-method intrinsic block must be contiguous up to ToBool");

//Phase 11 Step 4: fs namespace — namespace/directory/metadata only,
//never content (content IO lives in io). INTR_FS_* is taken by the
//FileStream intrinsics, so this block uses the FileSystem_ prefix.
static constexpr uint16_t INTR_FileSystem_Exists    = 120;
static constexpr uint16_t INTR_FileSystem_IsFile    = 121;
static constexpr uint16_t INTR_FileSystem_IsDir     = 122;
static constexpr uint16_t INTR_FileSystem_Size      = 123;
static constexpr uint16_t INTR_FileSystem_ListFiles = 124;
static constexpr uint16_t INTR_FileSystem_MakeDirs  = 125;
static constexpr uint16_t INTR_FileSystem_Remove    = 126;
static constexpr uint16_t INTR_FileSystem_Join      = 127;
static constexpr uint16_t kFileSystemIntrinsicFirst = 120;
static constexpr uint16_t kFileSystemIntrinsicCount = 8;
static_assert(INTR_FileSystem_Exists == kFileSystemIntrinsicFirst,
    "file-system intrinsic block must start at its First constant");
static_assert(INTR_FileSystem_Join
        == kFileSystemIntrinsicFirst + kFileSystemIntrinsicCount - 1,
    "file-system intrinsic block must be contiguous up to Join");

//Option B: per-formal default-value descriptor for cross-module import.
//Tag determines which payload field is meaningful:
//  RTK_Null   — null literal for any reference type (class/string/array). No payload.
//  RTK_Int32  — intValue holds the int32 default.
//  RTK_Float  — floatValue holds the float default.
//  RTK_String — stringIdx is an index into the PRODUCER module's stringConstants.
//               The consumer loader remaps this into its own stringConstants.
//  RTK_Long   — longValue holds the long default (0.7.5).
//  RTK_ULong  — longValue holds the ulong default's bit pattern (0.7.5).
//  RTK_Double — doubleValue holds the double default (0.7.5; payloads
//               exist ahead of double literals landing in Task 7).
//  RTK_Void   — sentinel: this formal has no default. Used to keep the vector
//               dense (always == paramCount entries; entries without defaults
//               carry RTK_Void so positional alignment is preserved).
//
//Constant-foldable negative int literals (`-5`) are folded at write time into
//a single RTK_Int32 with negative intValue — no separate tag needed.
struct DefaultValueDesc {
    uint8_t  tag = RTK_Void;
    uint32_t intValue = 0;     // RTK_Int32
    float    floatValue = 0.0f;// RTK_Float
    uint32_t stringIdx = 0;    // RTK_String (producer-side index)
    //0.7.5: 8-byte scalar channels (RTK_Long/RTK_ULong/RTK_Double).
    int64_t  longValue = 0;
    double   doubleValue = 0.0;

    bool hasDefault() const { return tag != RTK_Void; }
};

//Phase 9d: try/catch descriptor. One entry per catch clause. At throw time,
//VmExecutor linearly scans func.tryBlocks for the first entry where
//startPc <= opPc < endPc and (exceptionClassIdx == 0xFFFF or the thrown
//class is instance-of-target); on match, control jumps to handlerPc with
//the exception heap idx bound to locals[catchLocalOff].
//  exceptionClassIdx: 0xFFFF = unused/invalid (defensive default)
//                      any other value = CompiledClass index of catch type
//                      (Exception base class catches all subclasses via
//                      IsInstanceOrSubclass walk).
struct TryBlock {
    uint16_t startPc = 0;
    uint16_t endPc = 0;
    uint16_t handlerPc = 0;
    uint16_t exceptionClassIdx = 0xFFFF;
    uint16_t catchLocalOff = 0;
};

struct CompiledFunction {
    std::string name;
    std::vector<uint8_t> bytecode;
    std::vector<LocalDescriptor> locals;
    uint16_t localsSize = 0;
    uint16_t paramCount = 0;
    uint16_t returnTypeKind = 0;
    uint16_t intrinsicId = INTR_None;    //INTR_None = normal bytecode, else VM intrinsic
    //Phase 9f: native function declaration (`native int f(...);`). No
    //bytecode — OP_CallFunc dispatches by name through the host's
    //native table (VmExecutor::RegisterNative). Body-less by contract.
    bool isNative = false;
    //Option B: per-formal default values. Size == paramCount for free
    //functions and constructors; for methods, size == paramCount-1 (the
    //'this' slot has no default). Formals without defaults carry tag=RTK_Void.
    std::vector<DefaultValueDesc> defaultValues;
    //Phase 9d: try/catch table. Empty for functions without try blocks.
    std::vector<TryBlock> tryBlocks;
    //v1.9 (debugger): source file path this function was compiled from,
    //as recorded by the TU (empty for intrinsics/unknown). Survives the
    //import merge so breakpoints can address imported functions.
    std::string sourceFile;
    //v1.12: true formal types (one per AST formal — methods exclude the
    //implicit this slot, mirroring defaultValues' sizing) and the return
    //type. Empty / NonSerialized entries are "not expressible in the
    //descriptor grammar"; stub reconstruction degrades to the int32
    //placeholder for those. Intrinsics/builtins carry none (their records
    //are minted without the AST capture pass and never become stubs).
    std::vector<ParamTypeDesc> paramTypeDescs;
    TypeDesc returnTypeDesc;
};

struct CompiledModule;

struct CompiledClass {
    std::string name;
    uint16_t fieldCount = 0;
    int16_t superClassIdx = -1;
    std::vector<std::string> fieldNames;
    std::vector<uint16_t> fieldTypeKinds;       // RTK_* per field
    std::vector<uint16_t> fieldStructIndices;    // struct index for struct-typed fields, 0xFFFF for non-struct
    std::vector<uint16_t> fieldClassIndices;     // class index for class-typed fields, 0xFFFF for non-class
    std::vector<uint8_t>  fieldAccess;           // FA_* per field
    //v1.12: per-field recursive type descriptors (empty entries = not
    //expressible). Survive the import merge (indices remapped). Wire
    //fidelity only — like the struct sibling, no stub-side reader. Built-in
    //classes carry none (no AST capture; no consumer).
    std::vector<TypeDesc> fieldTypeDescs;
    std::vector<uint16_t> methodIndices;         // method function indices (by declaration order)
    uint16_t constructorIdx = 0xFFFF;           // constructor function index
};

struct CompiledModule {
    std::string name;
    std::vector<CompiledFunction> functions;
    std::vector<std::string> stringConstants;
    std::vector<CompiledStruct> structs;
    std::vector<CompiledClass> classes;
    std::vector<CompiledArrayType> arrayTypes;
    //Phase 8e-9b: per-enum value name tables. Outer index = enumDefIdx (in
    //declaration order across the module), inner index = enum int value.
    //Used by OP_Enum_to_str to render `Color.Red.toString()` → "Red".
    std::vector<std::vector<std::string>> enumNames;

    int FindFunction(const std::string& funcName) const {
        for (int i = 0; i < static_cast<int>(functions.size()); ++i)
            if (functions[i].name == funcName)
                return i;
        return -1;
    }

    int FindStruct(const std::string& structName) const {
        for (int i = 0; i < static_cast<int>(structs.size()); ++i)
            if (structs[i].name == structName)
                return i;
        return -1;
    }

    int FindClass(const std::string& className) const {
        for (int i = 0; i < static_cast<int>(classes.size()); ++i)
            if (classes[i].name == className)
                return i;
        return -1;
    }

    int FindArray(uint8_t elemKind, uint16_t elemTypeIdx) const {
        for (int i = 0; i < static_cast<int>(arrayTypes.size()); ++i)
            if (arrayTypes[i].elemKind == elemKind
                && arrayTypes[i].elemTypeIdx == elemTypeIdx)
                return i;
        return -1;
    }
};

//Serialize a CompiledModule to a stream in the current .nmod format.
//Single writer shared by VmBackend::SaveModule (ncc) and unit tests, so
//hand-written byte layouts can never drift from the reader again.
bool WriteCompiledModule(std::ostream& fs, const CompiledModule& mod);

} // namespace nlang
