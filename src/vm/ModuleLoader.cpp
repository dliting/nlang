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
    uint16_t majorVer = 0, minorVer = 0;
    fs.read(reinterpret_cast<char*>(&majorVer), sizeof(majorVer));
    fs.read(reinterpret_cast<char*>(&minorVer), sizeof(minorVer));
    //Truncated module detection — without this, an early EOF leaves
    //downstream counts (nameLen, strCount, ...) uninitialized and the
    //loader proceeds into resize() with garbage, producing confusing
    //"no 'main' function found" errors or worse.
    if (!fs.good())
        throw std::runtime_error("Truncated module file");
    //Phase 9d (v1.4): reject modules written by older ncc. v1.4 added
    //CompiledFunction.tryBlocks section; loading a v1.3 module would
    //misalign on the new section. Product hasn't shipped, so we refuse
    //stale modules outright instead of carrying forward-compat baggage.
    if (majorVer != 1 || minorVer < 4)
        throw std::runtime_error(
            "Module version " + std::to_string(majorVer) + "."
            + std::to_string(minorVer) + " is outdated; recompile with current ncc");

    // Module name
    uint32_t nameLen = 0;
    fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    if (!fs.good() || nameLen > (1u << 24))
        throw std::runtime_error("Invalid module: bad name length");
    mod.name.resize(nameLen);
    fs.read(mod.name.data(), nameLen);

    // String constants
    uint32_t strCount = 0;
    fs.read(reinterpret_cast<char*>(&strCount), sizeof(strCount));
    if (!fs.good() || strCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad string count");
    mod.stringConstants.resize(strCount);
    for (uint32_t i = 0; i < strCount; ++i) {
        uint32_t len = 0;
        fs.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!fs.good() || len > (1u << 24))
            throw std::runtime_error("Invalid module: bad string length");
        mod.stringConstants[i].resize(len);
        fs.read(mod.stringConstants[i].data(), len);
    }

    // Functions
    uint32_t funcCount = 0;
    fs.read(reinterpret_cast<char*>(&funcCount), sizeof(funcCount));
    if (!fs.good() || funcCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad function count");
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
        //intrinsicId was added in module format version 1.1.
        if (minorVer >= 1)
            fs.read(reinterpret_cast<char*>(&func.intrinsicId),
                    sizeof(func.intrinsicId));

        //Option B v1.3: per-formal default-value descriptors.
        uint16_t defaultCount = 0;
        fs.read(reinterpret_cast<char*>(&defaultCount), sizeof(defaultCount));
        if (!fs.good() || defaultCount > 256)
            throw std::runtime_error("Invalid module: bad default count");
        func.defaultValues.resize(defaultCount);
        for (uint16_t j = 0; j < defaultCount; ++j) {
            auto& dv = func.defaultValues[j];
            fs.read(reinterpret_cast<char*>(&dv.tag), sizeof(dv.tag));
            fs.read(reinterpret_cast<char*>(&dv.intValue), sizeof(dv.intValue));
            fs.read(reinterpret_cast<char*>(&dv.floatValue),
                    sizeof(dv.floatValue));
            fs.read(reinterpret_cast<char*>(&dv.stringIdx),
                    sizeof(dv.stringIdx));
        }

        uint32_t bcSize;
        fs.read(reinterpret_cast<char*>(&bcSize), sizeof(bcSize));
        func.bytecode.resize(bcSize);
        if (bcSize > 0)
            fs.read(reinterpret_cast<char*>(func.bytecode.data()), bcSize);

        //Phase 9d v1.4: try/catch table.
        uint16_t tryBlockCount = 0;
        fs.read(reinterpret_cast<char*>(&tryBlockCount), sizeof(tryBlockCount));
        if (!fs.good() || tryBlockCount > 1024)
            throw std::runtime_error("Invalid module: bad tryBlock count");
        func.tryBlocks.resize(tryBlockCount);
        for (uint16_t j = 0; j < tryBlockCount; ++j) {
            auto& tb = func.tryBlocks[j];
            fs.read(reinterpret_cast<char*>(&tb.startPc), sizeof(tb.startPc));
            fs.read(reinterpret_cast<char*>(&tb.endPc), sizeof(tb.endPc));
            fs.read(reinterpret_cast<char*>(&tb.handlerPc), sizeof(tb.handlerPc));
            fs.read(reinterpret_cast<char*>(&tb.exceptionClassIdx),
                    sizeof(tb.exceptionClassIdx));
            fs.read(reinterpret_cast<char*>(&tb.catchLocalOff),
                    sizeof(tb.catchLocalOff));
        }
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

    //Phase 8e-9b: enum name tables. Added in module format version 1.2.
    //Older modules (minorVer < 2) lack this section — leave enumNames empty,
    //which means OP_Enum_to_str cannot resolve; that's fine as no .nmod
    //predating 8e-9b would emit OP_Enum_to_str.
    if (minorVer >= 2) {
        uint32_t enumCount;
        fs.read(reinterpret_cast<char*>(&enumCount), sizeof(enumCount));
        if (!fs.good() || enumCount > (1u << 24))
            throw std::runtime_error("Invalid module: bad enum count");
        mod.enumNames.resize(enumCount);
        for (uint32_t i = 0; i < enumCount; ++i) {
            uint32_t valueCount;
            fs.read(reinterpret_cast<char*>(&valueCount), sizeof(valueCount));
            if (!fs.good() || valueCount > (1u << 24))
                throw std::runtime_error("Invalid module: bad enum value count");
            mod.enumNames[i].resize(valueCount);
            for (uint32_t j = 0; j < valueCount; ++j) {
                uint32_t len;
                fs.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (!fs.good() || len > (1u << 24))
                    throw std::runtime_error("Invalid module: bad enum name length");
                mod.enumNames[i][j].resize(len);
                fs.read(mod.enumNames[i][j].data(), len);
            }
        }
    }

    return mod;
}

} // namespace nlang
