/*---
    VecMathEntries.cpp — thin public entries over the active KernelOps
    backend table. All hardware knowledge lives behind the table (see
    TensorBackends.h); this file only forwards.
---*/
#include "VecMath.h"

#include "TensorBackends.h"

namespace tensor {

void VecExpF32(const float* src, float* dst, int64_t count) {
    ActiveKernelOps().expF32(src, dst, count);
}

void VecExpF64(const double* src, double* dst, int64_t count) {
    ActiveKernelOps().expF64(src, dst, count);
}

void VecTanhF32(const float* src, float* dst, int64_t count) {
    ActiveKernelOps().tanhF32(src, dst, count);
}

void VecTanhF64(const double* src, double* dst, int64_t count) {
    ActiveKernelOps().tanhF64(src, dst, count);
}

} // namespace tensor
