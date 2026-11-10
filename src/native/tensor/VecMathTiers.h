/*---
    VecMathTiers.h — internal (in-module) tier lane declarations for
    the vector math, elementwise and reduction families. The tier TUs
    (VecMathSse2/Avx2.cpp, VecBinarySse2/Avx2.cpp, VecReduceSse2/
    Avx2.cpp — x64 builds only) define these flat spans; the CPU
    backend's table entries dispatch ActiveSimdTier() onto them. Not
    installed; only the module's own TUs include it.
---*/
#pragma once

#include "TensorBackends.h"

#include <cstdint>

namespace tensor {
namespace detail {

//--- VecMath (transcendentals) ---
void ExpF32Sse2(const float* src, float* dst, int64_t count);
void ExpF32Avx2(const float* src, float* dst, int64_t count);
void TanhF32Sse2(const float* src, float* dst, int64_t count);
void TanhF32Avx2(const float* src, float* dst, int64_t count);

//--- binary / scalar-binary / clamp (flat spans) ---
void BinaryF32Sse2(BinaryKernelOp op, const float* a, const float* b,
                   float* out, int64_t count);
void BinaryF32Avx2(BinaryKernelOp op, const float* a, const float* b,
                   float* out, int64_t count);
void BinaryF64Sse2(BinaryKernelOp op, const double* a, const double* b,
                   double* out, int64_t count);
void BinaryF64Avx2(BinaryKernelOp op, const double* a, const double* b,
                   double* out, int64_t count);
void BinaryScalarF32Sse2(BinaryKernelOp op, const float* a, float scalar,
                         float* out, int64_t count);
void BinaryScalarF32Avx2(BinaryKernelOp op, const float* a, float scalar,
                         float* out, int64_t count);
void BinaryScalarF64Sse2(BinaryKernelOp op, const double* a, double scalar,
                         double* out, int64_t count);
void BinaryScalarF64Avx2(BinaryKernelOp op, const double* a, double scalar,
                         double* out, int64_t count);
void ClampF32Sse2(bool isMax, const float* src, float bound, float* out,
                  int64_t count);
void ClampF32Avx2(bool isMax, const float* src, float bound, float* out,
                  int64_t count);
void ClampF64Sse2(bool isMax, const double* src, double bound, double* out,
                  int64_t count);
void ClampF64Avx2(bool isMax, const double* src, double bound, double* out,
                  int64_t count);

//--- reductions (flat spans) ---
void SumF32Sse2(const float* cells, int64_t count, double* out);
void SumF32Avx2(const float* cells, int64_t count, double* out);
void SumF64Sse2(const double* cells, int64_t count, double* out);
void SumF64Avx2(const double* cells, int64_t count, double* out);
void DotF32Sse2(const float* a, const float* b, int64_t count, float* out);
void DotF32Avx2(const float* a, const float* b, int64_t count, float* out);
void DotF64Sse2(const double* a, const double* b, int64_t count,
                double* out);
void DotF64Avx2(const double* a, const double* b, int64_t count,
                double* out);
void MaxF32Sse2(const float* cells, int64_t count, float* out);
void MaxF32Avx2(const float* cells, int64_t count, float* out);
void MaxF64Sse2(const double* cells, int64_t count, double* out);
void MaxF64Avx2(const double* cells, int64_t count, double* out);
void MinF32Sse2(const float* cells, int64_t count, float* out);
void MinF32Avx2(const float* cells, int64_t count, float* out);
void MinF64Sse2(const double* cells, int64_t count, double* out);
void MinF64Avx2(const double* cells, int64_t count, double* out);
void AddIntoF64FromF32Sse2(double* acc, const float* row, int64_t count);
void AddIntoF64FromF32Avx2(double* acc, const float* row, int64_t count);
void AddIntoF64Sse2(double* acc, const double* row, int64_t count);
void AddIntoF64Avx2(double* acc, const double* row, int64_t count);

} // namespace detail
} // namespace tensor
