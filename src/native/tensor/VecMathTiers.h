/*---
    VecMathTiers.h — internal (in-module) tier lane declarations. Each
    tier TU (VecMathSse2.cpp / VecMathAvx2.cpp, x64 builds only) defines
    these flat spans; the public VecMath entries dispatch
    ActiveSimdTier() onto them. Not installed; only the module's own TUs
    include it.
---*/
#pragma once

#include <cstdint>

namespace tensor {
namespace detail {

void ExpF32Sse2(const float* src, float* dst, int64_t count);
void ExpF32Avx2(const float* src, float* dst, int64_t count);
void TanhF32Sse2(const float* src, float* dst, int64_t count);
void TanhF32Avx2(const float* src, float* dst, int64_t count);

} // namespace detail
} // namespace tensor
