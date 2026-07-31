#pragma once
#include "CompiledModule.h"
#include "BytecodeReader.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

class VmExecutor {
public:
    int Execute(const CompiledModule& module);

private:
    void ExecuteFunction(const CompiledFunction& func,
        uint8_t* pResult, uint8_t* locals);

    static const size_t RECURSE_LIMIT = 1000;
    size_t m_recurseDepth = 0;
    const CompiledModule* m_currModule = nullptr;
    std::vector<std::string> m_stringPool;
};

} // namespace nlang
