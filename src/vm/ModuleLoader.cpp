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

    return mod;
}

} // namespace nlang
