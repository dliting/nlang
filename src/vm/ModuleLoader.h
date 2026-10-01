#pragma once
#include "nlang/vm/CompiledModule.h"
#include <string>

namespace nlang {

class ModuleLoader {
public:
    static CompiledModule Load(const std::string& filePath);
    //Same parse, from an in-memory .ncu image (a package member extracted
    //by the .npkg loader). filePath is kept for diagnostics only - the
    //bytes are the source of truth.
    static CompiledModule LoadFromBytes(const std::string& filePath,
                                        const std::string& bytes);
};

} // namespace nlang
