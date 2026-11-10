/*---
    KernelOpsCpu.cpp — the built-in CPU backend: the KernelOps table
    entries dispatch the SIMD tier per call (scalar lanes live here as
    the portable reference; the x64 tier lanes sit in the VecMath tier
    TUs). This is the only backend in this iteration; device backends
    register their own tables (see TensorBackends.h).
---*/
#include "TensorBackends.h"

#include "SimdCpu.h"

#if defined(NLANG_SIMD_X64)
#include "VecMathTiers.h"
#endif

#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace tensor {

namespace {

//The scalar lanes compute in double and narrow — the same rounding the
//accuracy contract's reference uses, so this tier matches it exactly.
void CpuExpF32(const float* src, float* dst, int64_t count) {
    for (int64_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(std::exp(static_cast<double>(src[i])));
}

void CpuExpF64(const double* src, double* dst, int64_t count) {
    for (int64_t i = 0; i < count; ++i)
        dst[i] = std::exp(src[i]);
}

void CpuTanhF32(const float* src, float* dst, int64_t count) {
    for (int64_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(std::tanh(static_cast<double>(src[i])));
}

void CpuTanhF64(const double* src, double* dst, int64_t count) {
    for (int64_t i = 0; i < count; ++i)
        dst[i] = std::tanh(src[i]);
}

const KernelOps kCpuKernelOps = {
    &CpuExpF32,
    &CpuExpF64,
    &CpuTanhF32,
    &CpuTanhF64,
};

using KernelOpsRegistry = std::map<std::string, const KernelOps*>;

KernelOpsRegistry& KernelOpsTable() {
    static KernelOpsRegistry registry = {
        {"cpu", &kCpuKernelOps},
    };
    return registry;
}

} // namespace

const KernelOps& ActiveKernelOps() {
    static const KernelOps* cached = [] {
        const char* wanted = std::getenv("NLANG_TENSOR_BACKEND");
        if (wanted != nullptr) {
            const auto& registry = KernelOpsTable();
            const auto it = registry.find(wanted);
            if (it != registry.end())
                return it->second;
            //Unknown names fall through to the built-in CPU backend.
        }
        return &kCpuKernelOps;
    }();
    return *cached;
}

void RegisterKernelOps(const char* name, const KernelOps* ops) {
    KernelOpsTable()[name] = ops;
}

} // namespace tensor
