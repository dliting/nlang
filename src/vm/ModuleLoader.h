#pragma once
#include "nlang/vm/CompiledModule.h"
#include <string>

namespace nlang {

class ModuleLoader {
public:
    //entryKeyOut (optional) receives the serialized entry key ("" = the
    //unit declares no entry) — the runtime linker needs the key itself,
    //not just the resolved own-table index.
    static CompiledModule Load(const std::string& filePath,
                               std::string* entryKeyOut = nullptr);
    //Same parse, from an in-memory .ncu image (a package member extracted
    //by the .npkg loader). filePath is kept for diagnostics only - the
    //bytes are the source of truth.
    static CompiledModule LoadFromBytes(const std::string& filePath,
                                        const std::string& bytes,
                                        std::string* entryKeyOut = nullptr);
};

} // namespace nlang
