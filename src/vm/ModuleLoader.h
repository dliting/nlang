#pragma once
#include "CompiledModule.h"
#include <string>

namespace nlang {

class ModuleLoader {
public:
    static CompiledModule Load(const std::string& filePath);
};

} // namespace nlang
