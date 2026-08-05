#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nlang {

//Runtime type kind constants for serialization.
//Compile-time NK_* values exceed uint8_t range, so we map them.
static constexpr uint8_t RTK_Int32  = 0;
static constexpr uint8_t RTK_Float  = 1;
static constexpr uint8_t RTK_String = 2;
static constexpr uint8_t RTK_Struct = 3;
static constexpr uint8_t RTK_Class  = 4;
static constexpr uint8_t RTK_Array  = 5;
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

struct CompiledFunction {
    std::string name;
    std::vector<uint8_t> bytecode;
    std::vector<LocalDescriptor> locals;
    uint16_t localsSize = 0;
    uint16_t paramCount = 0;
    uint16_t returnTypeKind = 0;
    uint16_t intrinsicId = INTR_None;    //INTR_None = normal bytecode, else VM intrinsic
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

} // namespace nlang
