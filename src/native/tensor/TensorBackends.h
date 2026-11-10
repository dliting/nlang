/*---
    TensorBackends.h — the kernel-backend seam of the tensor native
    module. Every compute family the elementwise driver, matmul and
    reductions need is a function-pointer slot in KernelOps; callers
    look up ActiveKernelOps() once per module call and never learn what
    hardware is underneath. Two orthogonal extension axes (2026-11-11
    tensor vector ops spec §4.1):

    - ISA tiers INSIDE a backend: the built-in CPU backend dispatches
      Scalar/Sse2/Avx2 per entry (a future AVX-512 tier is a new
      SimdTier value + TU — no interface change);
    - device BACKENDS as whole tables: an AMD GPU/HIP (or any other)
      backend implements the full KernelOps in its own TU/DLL and
      registers it by name; NLANG_TENSOR_BACKEND selects it. Only the
      CPU table ships in this iteration — the registry and the selector
      are real but the second backend is future work.
---*/
#pragma once

#include <cstdint>

namespace tensor {

struct KernelOps {
    //VecMath family (flat spans; count <= 0 must be a no-op).
    void (*expF32)(const float* src, float* dst, int64_t count);
    void (*expF64)(const double* src, double* dst, int64_t count);
    void (*tanhF32)(const float* src, float* dst, int64_t count);
    void (*tanhF64)(const double* src, double* dst, int64_t count);
    //MatMul family: dense row-major C[rows, cols] = A[rows, inner] ·
    //B[inner, cols], ACCUMULATED into out — the caller passes zeroed
    //storage (the driver mints fresh zeroed arrays; the scalar loop
    //has always read back outCells while accumulating). Kernels may
    //overwrite or read-modify-write out cells, never other memory.
    //rows/inner/cols == 0 yields no work. Ints wrap like language int
    //arithmetic; the k accumulation order is the scalar i-k-j order
    //for every lane (float/double differ only by FMA rounding).
    void (*matmulF32)(int32_t rows, int32_t inner, int32_t cols,
                      const float* a, const float* b, float* out);
    void (*matmulF64)(int32_t rows, int32_t inner, int32_t cols,
                      const double* a, const double* b, double* out);
    void (*matmulI32)(int32_t rows, int32_t inner, int32_t cols,
                      const int32_t* a, const int32_t* b, int32_t* out);
    void (*matmulI64)(int32_t rows, int32_t inner, int32_t cols,
                      const int64_t* a, const int64_t* b, int64_t* out);
    //Later families (binary lanes, reductions) join here as the driver
    //grows — the table stays the only hardware-facing surface.
};

//The active table: NLANG_TENSOR_BACKEND names a registered backend
//(cached on first use); unset or unknown names resolve to the
//built-in CPU backend.
const KernelOps& ActiveKernelOps();

//Registration hook for backends defined outside this TU (a device
//DLL's static init, or tests). Name must stay alive.
void RegisterKernelOps(const char* name, const KernelOps* ops);

} // namespace tensor
