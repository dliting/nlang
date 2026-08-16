#pragma once
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace nlang {

//.nmod format version. Single source of truth shared by the writer
//(WriteCompiledModule in ModuleSaver.cpp) and the reader (ModuleLoader) —
//bump both sides atomically by editing only these constants.
inline constexpr uint16_t NMOD_FORMAT_MAJOR = 1;
inline constexpr uint16_t NMOD_FORMAT_MINOR = 6;

//Runtime type kind constants for serialization.
//Compile-time NK_* values exceed uint8_t range, so we map them.
static constexpr uint8_t RTK_Int32  = 0;
static constexpr uint8_t RTK_Float  = 1;
static constexpr uint8_t RTK_String = 2;
static constexpr uint8_t RTK_Struct = 3;
static constexpr uint8_t RTK_Class  = 4;
static constexpr uint8_t RTK_Array  = 5;
static constexpr uint8_t RTK_Boxed  = 6;  //Phase 8e-1: boxed primitive (slot[0]=tag, slot[1]=value)
static constexpr uint8_t RTK_Null   = 0xFD;  //Option B: null default value (any reference type)
static constexpr uint8_t RTK_Unfoldable = 0xFC;  //Option B: had default but not constant-foldable
static constexpr uint8_t RTK_Void   = 0xFE;  //used for ctor/void method stubs

struct LocalDescriptor {
    uint16_t offset = 0;
    uint16_t size = 0;
    uint8_t  isParam = 0;
    uint8_t  typeKind = 0;
    std::string name;
};

struct CompiledStruct {
    std::string name;
    uint16_t fieldCount = 0;
    std::vector<std::string> fieldNames;
    std::vector<uint16_t> fieldTypeKinds;       // RTK_* per field
    std::vector<uint16_t> fieldStructIndices;    // struct index for struct-typed fields, 0xFFFF for non-struct
    std::vector<uint16_t> fieldClassIndices;     // class index for class-typed fields, 0xFFFF for non-class
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

//Option B: per-formal default-value descriptor for cross-module import.
//Tag determines which payload field is meaningful:
//  RTK_Null   — null literal for any reference type (class/string/array). No payload.
//  RTK_Int32  — intValue holds the int32 default.
//  RTK_Float  — floatValue holds the float default.
//  RTK_String — stringIdx is an index into the PRODUCER module's stringConstants.
//               The consumer loader remaps this into its own string pool.
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
