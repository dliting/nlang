#include "ModuleLoader.h"
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace nlang {

CompiledModule ModuleLoader::Load(const std::string& filePath) {
    std::ifstream fs(filePath, std::ios::binary);
    if (!fs.is_open())
        throw std::runtime_error("Failed to open module file: " + filePath);

    CompiledModule mod;

    // Magic
    char magic[8] = {};
    fs.read(magic, 8);
    if (std::memcmp(magic, "NLANGMOD", 8) != 0)
        throw std::runtime_error("Invalid module file format");

    // Version
    uint16_t majorVer, minorVer;
    fs.read(reinterpret_cast<char*>(&majorVer), sizeof(majorVer));
    fs.read(reinterpret_cast<char*>(&minorVer), sizeof(minorVer));

    // Module name
    uint32_t nameLen;
    fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    mod.name.resize(nameLen);
    fs.read(mod.name.data(), nameLen);

    // String constants
    uint32_t strCount;
    fs.read(reinterpret_cast<char*>(&strCount), sizeof(strCount));
    mod.stringConstants.resize(strCount);
    for (uint32_t i = 0; i < strCount; ++i) {
        uint32_t len;
        fs.read(reinterpret_cast<char*>(&len), sizeof(len));
        mod.stringConstants[i].resize(len);
        fs.read(mod.stringConstants[i].data(), len);
    }

    // Functions
    uint32_t funcCount;
    fs.read(reinterpret_cast<char*>(&funcCount), sizeof(funcCount));
    mod.functions.resize(funcCount);

    for (uint32_t i = 0; i < funcCount; ++i) {
        auto& func = mod.functions[i];

        uint32_t fnameLen;
        fs.read(reinterpret_cast<char*>(&fnameLen), sizeof(fnameLen));
        func.name.resize(fnameLen);
        fs.read(func.name.data(), fnameLen);

        fs.read(reinterpret_cast<char*>(&func.localsSize),
                sizeof(func.localsSize));
        fs.read(reinterpret_cast<char*>(&func.paramCount),
                sizeof(func.paramCount));
        fs.read(reinterpret_cast<char*>(&func.returnTypeKind),
                sizeof(func.returnTypeKind));

        uint32_t bcSize;
        fs.read(reinterpret_cast<char*>(&bcSize), sizeof(bcSize));
        func.bytecode.resize(bcSize);
        if (bcSize > 0)
            fs.read(reinterpret_cast<char*>(func.bytecode.data()), bcSize);
    }

    // Struct descriptors
    uint32_t structCount;
    fs.read(reinterpret_cast<char*>(&structCount), sizeof(structCount));
    mod.structs.resize(structCount);

    for (uint32_t i = 0; i < structCount; ++i) {
        auto& st = mod.structs[i];

        uint32_t stNameLen;
        fs.read(reinterpret_cast<char*>(&stNameLen), sizeof(stNameLen));
        st.name.resize(stNameLen);
        fs.read(st.name.data(), stNameLen);

        fs.read(reinterpret_cast<char*>(&st.fieldCount),
                sizeof(st.fieldCount));

        st.fieldNames.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            uint32_t fnLen;
            fs.read(reinterpret_cast<char*>(&fnLen), sizeof(fnLen));
            st.fieldNames[j].resize(fnLen);
            fs.read(st.fieldNames[j].data(), fnLen);
        }

        st.fieldTypeKinds.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldTypeKinds[j]),
                    sizeof(st.fieldTypeKinds[j]));
        }

        st.fieldStructIndices.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldStructIndices[j]),
                    sizeof(st.fieldStructIndices[j]));
        }

        st.fieldClassIndices.resize(st.fieldCount);
        for (uint16_t j = 0; j < st.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&st.fieldClassIndices[j]),
                    sizeof(st.fieldClassIndices[j]));
        }
    }

    // Class descriptors
    uint32_t classCount;
    fs.read(reinterpret_cast<char*>(&classCount), sizeof(classCount));
    mod.classes.resize(classCount);

    for (uint32_t i = 0; i < classCount; ++i) {
        auto& cc = mod.classes[i];

        uint32_t ccNameLen;
        fs.read(reinterpret_cast<char*>(&ccNameLen), sizeof(ccNameLen));
        cc.name.resize(ccNameLen);
        fs.read(cc.name.data(), ccNameLen);

        fs.read(reinterpret_cast<char*>(&cc.fieldCount),
                sizeof(cc.fieldCount));
        fs.read(reinterpret_cast<char*>(&cc.superClassIdx),
                sizeof(cc.superClassIdx));

        cc.fieldNames.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            uint32_t fnLen;
            fs.read(reinterpret_cast<char*>(&fnLen), sizeof(fnLen));
            cc.fieldNames[j].resize(fnLen);
            fs.read(cc.fieldNames[j].data(), fnLen);
        }

        cc.fieldTypeKinds.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldTypeKinds[j]),
                    sizeof(cc.fieldTypeKinds[j]));
        }

        cc.fieldStructIndices.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldStructIndices[j]),
                    sizeof(cc.fieldStructIndices[j]));
        }

        cc.fieldClassIndices.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldClassIndices[j]),
                    sizeof(cc.fieldClassIndices[j]));
        }

        cc.fieldAccess.resize(cc.fieldCount);
        for (uint16_t j = 0; j < cc.fieldCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.fieldAccess[j]),
                    sizeof(cc.fieldAccess[j]));
        }

        //Method indices
        uint16_t methodCount;
        fs.read(reinterpret_cast<char*>(&methodCount), sizeof(methodCount));
        cc.methodIndices.resize(methodCount);
        for (uint16_t j = 0; j < methodCount; ++j) {
            fs.read(reinterpret_cast<char*>(&cc.methodIndices[j]),
                    sizeof(cc.methodIndices[j]));
        }

        //Constructor index
        fs.read(reinterpret_cast<char*>(&cc.constructorIdx),
                sizeof(cc.constructorIdx));
    }

    //Array type descriptors
    uint32_t arrayTypeCount;
    fs.read(reinterpret_cast<char*>(&arrayTypeCount), sizeof(arrayTypeCount));
    mod.arrayTypes.resize(arrayTypeCount);
    for (uint32_t i = 0; i < arrayTypeCount; ++i) {
        fs.read(reinterpret_cast<char*>(&mod.arrayTypes[i].elemKind),
                sizeof(mod.arrayTypes[i].elemKind));
        fs.read(reinterpret_cast<char*>(&mod.arrayTypes[i].elemTypeIdx),
                sizeof(mod.arrayTypes[i].elemTypeIdx));
    }

    return mod;
}

} // namespace nlang
