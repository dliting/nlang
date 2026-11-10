/*---
    MatMulTiers.h — internal (in-module) matmul tier lane declarations.
    The tier TUs (MatMulSse2.cpp / MatMulAvx2.cpp, x64 builds only)
    define these dense row-major kernels; the CPU backend's table
    entries dispatch ActiveSimdTier() onto them. Not installed; only
    the module's own TUs include it.
---*/
#pragma once

#include <cstdint>

namespace tensor {
namespace detail {

void MatMulF32Sse2(int32_t rows, int32_t inner, int32_t cols,
                   const float* a, const float* b, float* out);
void MatMulF32Avx2(int32_t rows, int32_t inner, int32_t cols,
                   const float* a, const float* b, float* out);
void MatMulF64Sse2(int32_t rows, int32_t inner, int32_t cols,
                   const double* a, const double* b, double* out);
void MatMulF64Avx2(int32_t rows, int32_t inner, int32_t cols,
                   const double* a, const double* b, double* out);

} // namespace detail
} // namespace tensor
