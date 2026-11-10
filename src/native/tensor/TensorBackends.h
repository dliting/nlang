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

    Numeric contracts shared by every backend (the scalar lanes are the
    authority): floats follow IEEE (div by zero yields inf/nan, no
    raise — only the language's int div raises); unary/binary lane math
    matches the scalar lanes within the VecMath tolerance contract;
    max/min keep the scalar walk's exact order rule (a NaN cell never
    wins unless it was the first element; ties keep the earlier
    element). Only float/double ride the table — int kernels keep the
    language's wrap-and-raise semantics in their scalar loops.
---*/
#pragma once

#include <cstdint>

namespace tensor {

enum class UnaryKernelOp : int32_t { Exp, Log, Tanh, Neg };
//Rsub/Rdiv keep the scalar on the LEFT (s - a[i], s / a[i]);
//ReLUGrad is the ReLU gate backward: b > 0 ? a : 0 (b is the
//pre-activation, a the outgoing gradient).
enum class BinaryKernelOp : int32_t
    { Add, Sub, Mul, Div, Rsub, Rdiv, ReLUGrad };

struct KernelOps {
    //Elementwise family: flat spans, count <= 0 must be a no-op.
    void (*unaryF32)(UnaryKernelOp op, const float* src, float* dst,
                     int64_t count);
    void (*unaryF64)(UnaryKernelOp op, const double* src, double* dst,
                     int64_t count);
    void (*binaryF32)(BinaryKernelOp op, const float* a, const float* b,
                      float* out, int64_t count);
    void (*binaryF64)(BinaryKernelOp op, const double* a, const double* b,
                      double* out, int64_t count);
    void (*binaryScalarF32)(BinaryKernelOp op, const float* a,
                            float scalar, float* out, int64_t count);
    void (*binaryScalarF64)(BinaryKernelOp op, const double* a,
                            double scalar, double* out, int64_t count);
    //Clamp family (max(a, bound) / min(a, bound) — relu rides max).
    void (*clampF32)(bool isMax, const float* src, float bound,
                     float* out, int64_t count);
    void (*clampF64)(bool isMax, const double* src, double bound,
                     double* out, int64_t count);
    //Reduction family. sum accumulates in double (the language rule,
    //order-free for the exact sums the e2e suite pins); dot keeps its
    //carrier accumulator (float dot accumulates float); max/min
    //require count > 0 (the driver guards empty arrays).
    void (*sumF32)(const float* cells, int64_t count, double* out);
    void (*sumF64)(const double* cells, int64_t count, double* out);
    void (*dotF32)(const float* a, const float* b, int64_t count,
                   float* out);
    void (*dotF64)(const double* a, const double* b, int64_t count,
                   double* out);
    void (*maxF32)(const float* cells, int64_t count, float* out);
    void (*maxF64)(const double* cells, int64_t count, double* out);
    void (*minF32)(const float* cells, int64_t count, float* out);
    void (*minF64)(const double* cells, int64_t count, double* out);
    //Axis-0 accumulation for sum(a, axis=0) fast paths: acc[j] += row[j]
    //in double (mirrors the per-cell double accumulator of the scalar
    //walk).
    void (*addIntoF64FromF32)(double* acc, const float* row,
                              int64_t count);
    void (*addIntoF64)(double* acc, const double* row, int64_t count);
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
};

//The active table: NLANG_TENSOR_BACKEND names a registered backend
//(cached on first use); unset or unknown names resolve to the
//built-in CPU backend.
const KernelOps& ActiveKernelOps();

//Registration hook for backends defined outside this TU (a device
//DLL's static init, or tests). Name must stay alive.
void RegisterKernelOps(const char* name, const KernelOps* ops);

} // namespace tensor
