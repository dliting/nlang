#include "ModuleLoader.h"
#include "ModuleLoaderRecords.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace nlang {

CompiledModule ModuleLoader::Load(const std::string& filePath) {
    std::ifstream fs(filePath, std::ios::binary);
    if (!fs.is_open())
        throw std::runtime_error("Failed to open module file: " + filePath);
    std::stringstream buffer;
    buffer << fs.rdbuf();
    return LoadFromBytes(filePath, buffer.str());
}

CompiledModule ModuleLoader::LoadFromBytes(const std::string& filePath,
                                           const std::string& bytes) {
    std::istringstream fs(bytes);

    CompiledModule mod;

    // Magic
    char magic[8] = {};
    fs.read(magic, 8);
    if (std::memcmp(magic, NCU_MAGIC, sizeof(NCU_MAGIC)) != 0)
        throw std::runtime_error("Invalid module file format");

    // Version
    uint16_t majorVer = 0, minorVer = 0;
    fs.read(reinterpret_cast<char*>(&majorVer), sizeof(majorVer));
    fs.read(reinterpret_cast<char*>(&minorVer), sizeof(minorVer));
    //Truncation check: without it an early EOF leaves downstream counts
    //uninitialized and the loader resizes with garbage.
    if (!fs.good())
        throw std::runtime_error("Truncated module file");
    //v2.0: the module's dotted path (package identity of the unit).
    uint16_t modulePathLen = 0;
    fs.read(reinterpret_cast<char*>(&modulePathLen), sizeof(modulePathLen));
    std::string modulePath(modulePathLen, '\0');
    fs.read(modulePath.data(), modulePathLen);
    mod.modulePath = std::move(modulePath);
    if (!fs.good())
        throw std::runtime_error("Truncated module file");
    //v2.0: floor and ceiling pin the current version exactly — a v1.x
    //module misparses at the first 2.0 field (module path); the older
    //per-field format gates were removed with the 2.0 bump.
    const uint16_t kCurrentMajorVer = NCU_FORMAT_MAJOR;
    const uint16_t kCurrentMinorVer = NCU_FORMAT_MINOR;
    if (majorVer < kCurrentMajorVer
        || (majorVer == kCurrentMajorVer && minorVer < kCurrentMinorVer))
        throw std::runtime_error(
            "Module version " + std::to_string(majorVer) + "."
            + std::to_string(minorVer) + " is outdated; recompile with current ncc");
    if (majorVer > kCurrentMajorVer || minorVer > kCurrentMinorVer)
        throw std::runtime_error(
            "Module version " + std::to_string(majorVer) + "."
            + std::to_string(minorVer)
            + " was written by a newer ncc; upgrade ncc/nvm to run it");

    // Module name
    uint32_t nameLen = 0;
    fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
    if (!fs.good() || nameLen > (1u << 24))
        throw std::runtime_error("Invalid module: bad name length");
    mod.name.resize(nameLen);
    fs.read(mod.name.data(), nameLen);

    //v2.0: the entry function's qualified key (empty = no entry). The
    //index is re-resolved by name at the end of the parse — table-
    //layout-dependent indices are not serializable; the .npkg entry
    //record may re-point it after LoadFromBytes.
    uint16_t entryKeyLen = 0;
    fs.read(reinterpret_cast<char*>(&entryKeyLen), sizeof(entryKeyLen));
    if (!fs.good())
        throw std::runtime_error("Invalid module: truncated entry key");
    std::string entryKey(entryKeyLen, ' ');
    fs.read(entryKey.data(), entryKeyLen);

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

    //Composite record tables parse in this stream order; the per-field
    //parsers live in ModuleLoaderRecords.cpp (source-size split, zero
    //behavior change).
    ReadFunctionRecords(fs, mod);
    ReadStructRecords(fs, mod);
    ReadClassRecords(fs, mod);

    //v1.12: descriptors reference struct/class indices of THIS module —
    //the function records were parsed before the tables, so bounds are
    //only checkable now. Any violation is a corrupt or hostile module.
    //v1.13: the version gate is gone — the floor subsumes 12.
    {
        for (const auto& func : mod.functions) {
            for (const auto& ptd : func.paramTypeDescs)
                ValidateTypeDescIndices(ptd.type, mod.structs.size(),
                    mod.classes.size());
            ValidateTypeDescIndices(func.returnTypeDesc, mod.structs.size(),
                mod.classes.size());
        }
        for (const auto& st : mod.structs)
            for (const auto& td : st.fieldTypeDescs)
                ValidateTypeDescIndices(td, mod.structs.size(),
                    mod.classes.size());
        for (const auto& cc : mod.classes)
            for (const auto& td : cc.fieldTypeDescs)
                ValidateTypeDescIndices(td, mod.structs.size(),
                    mod.classes.size());
    }

    //Array type descriptors
    uint32_t arrayTypeCount;
    fs.read(reinterpret_cast<char*>(&arrayTypeCount), sizeof(arrayTypeCount));
    if (!fs.good() || arrayTypeCount > (1u << 24))
        throw std::runtime_error("Invalid module: bad array type count");
    mod.arrayTypes.resize(arrayTypeCount);
    for (uint32_t i = 0; i < arrayTypeCount; ++i) {
        fs.read(reinterpret_cast<char*>(&mod.arrayTypes[i].elemKind),
                sizeof(mod.arrayTypes[i].elemKind));
        fs.read(reinterpret_cast<char*>(&mod.arrayTypes[i].elemTypeIdx),
                sizeof(mod.arrayTypes[i].elemTypeIdx));
    }

    //Phase 8e-9b: enum name tables.
    {

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
        //v2.0 phase 6: qualified keys, one per enum (same count — the
        //writer emits exactly enumCount keys right after the name tables).
        mod.enumKeys.resize(enumCount);
        for (uint32_t i = 0; i < enumCount; ++i) {
            uint16_t len;
            fs.read(reinterpret_cast<char*>(&len), sizeof(len));
            if (!fs.good())
                throw std::runtime_error(
                    "Invalid module: truncated enum keys");
            mod.enumKeys[i].resize(len);
            fs.read(mod.enumKeys[i].data(), len);
        }
        if (!fs.good())
            throw std::runtime_error("Invalid module: truncated enum keys");
    }

    //v2.0 phase 6: cross-unit import slots, one section per table (in
    //slot order — own entries occupy 0..n-1, imports n..n+m). Per entry:
    //u16 modulePathLen + bytes, u16 nameLen + bytes, u32 paramCount; the
    //function section additionally carries u16 ownerClassKeyLen + bytes
    //(empty = namespace-level function; see SymbolImport).
    auto readImports = [&fs](std::vector<CompiledModule::SymbolImport>& imports,
                             bool withOwnerKey)
    {
        uint32_t count;
        fs.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!fs.good() || count > (1u << 24))
            throw std::runtime_error("Invalid module: bad import count");
        imports.resize(count);
        for (auto& imp : imports)
        {
            uint16_t modulePathLen;
            fs.read(reinterpret_cast<char*>(&modulePathLen),
                    sizeof(modulePathLen));
            if (!fs.good())
                throw std::runtime_error(
                    "Invalid module: bad import module path length");
            imp.modulePath.resize(modulePathLen);
            fs.read(imp.modulePath.data(), modulePathLen);
            uint16_t nameLen;
            fs.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
            if (!fs.good())
                throw std::runtime_error(
                    "Invalid module: bad import name length");
            imp.name.resize(nameLen);
            fs.read(imp.name.data(), nameLen);
            fs.read(reinterpret_cast<char*>(&imp.paramCount),
                    sizeof(imp.paramCount));
            if (!fs.good())
                throw std::runtime_error("Invalid module: truncated imports");
            if (withOwnerKey)
            {
                uint16_t ownerLen;
                fs.read(reinterpret_cast<char*>(&ownerLen), sizeof(ownerLen));
                if (!fs.good())
                    throw std::runtime_error(
                        "Invalid module: bad import owner key length");
                imp.ownerClassKey.resize(ownerLen);
                fs.read(imp.ownerClassKey.data(), ownerLen);
            }
        }
    };
    readImports(mod.functionImports, true);
    readImports(mod.classImports, false);
    readImports(mod.structImports, false);
    readImports(mod.enumImports, false);
    if (!fs.good())
        throw std::runtime_error("Invalid module: truncated import sections");

    //Resolve the serialized entry key to an index. An empty key is the
    //"no entry" contract (libraries); a NON-empty key that names no
    //function is a corrupt or hostile module — the producer wrote it
    //from its own table — so refuse it here rather than degrade to a
    //generic no-entry error at run time.
    if (entryKey.empty()) {
        mod.entryPoint = -1;
    } else {
        const int entryIdx = mod.FindFunction(entryKey);
        if (entryIdx < 0)
            throw std::runtime_error(
                "Invalid module: entry key '" + entryKey
                + "' names no function in the module");
        mod.entryPoint = entryIdx;
    }

    return mod;
}

} // namespace nlang
