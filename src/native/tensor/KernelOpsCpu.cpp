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
#include "MatMulTiers.h"
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

//Scalar matmul (the portable reference and the fallback tier): plain
//i-k-j — the exact accumulation order the vector kernels preserve.
template <typename T>
void CpuMatMul(int32_t rows, int32_t inner, int32_t cols,
               const T* a, const T* b, T* out) {
    for (int32_t i = 0; i < rows; ++i) {
        for (int32_t k = 0; k < inner; ++k) {
            const T aik = a[static_cast<int64_t>(i) * inner + k];
            for (int32_t j = 0; j < cols; ++j) {
                T& acc = out[static_cast<int64_t>(i) * cols + j];
                acc = static_cast<T>(
                    acc + aik * b[static_cast<int64_t>(k) * cols + j]);
            }
        }
    }
}

void CpuMatMulF32(int32_t rows, int32_t inner, int32_t cols,
                  const float* a, const float* b, float* out) {
#if defined(NLANG_SIMD_X64)
    switch (ActiveSimdTier()) {
        case SimdTier::Avx2:
            detail::MatMulF32Avx2(rows, inner, cols, a, b, out);
            return;
        case SimdTier::Sse2:
            detail::MatMulF32Sse2(rows, inner, cols, a, b, out);
            return;
        case SimdTier::Scalar:
            break;
    }
#endif
    CpuMatMul(rows, inner, cols, a, b, out);
}

void CpuMatMulF64(int32_t rows, int32_t inner, int32_t cols,
                  const double* a, const double* b, double* out) {
#if defined(NLANG_SIMD_X64)
    switch (ActiveSimdTier()) {
        case SimdTier::Avx2:
            detail::MatMulF64Avx2(rows, inner, cols, a, b, out);
            return;
        case SimdTier::Sse2:
            detail::MatMulF64Sse2(rows, inner, cols, a, b, out);
            return;
        case SimdTier::Scalar:
            break;
    }
#endif
    CpuMatMul(rows, inner, cols, a, b, out);
}

const KernelOps kCpuKernelOps = {
    &CpuExpF32,
    &CpuExpF64,
    &CpuTanhF32,
    &CpuTanhF64,
    &CpuMatMulF32,
    &CpuMatMulF64,
    &CpuMatMul<int32_t>,
    &CpuMatMul<int64_t>,
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
