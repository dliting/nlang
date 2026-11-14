/*---
    TensorBackends.h — the CPU-internal SIMD tier seam of the tensor
    native module (P1 of the v0.14.x device line retired this table from
    the backend-registration role). Every compute family the elementwise
    driver, matmul and reductions need is a function-pointer slot in
    KernelOps; the CPU backend's TensorOps implementation (tensor_ops_
    cpu.cpp, the whole-op contract in TensorOps.h) calls these per flat
    span, dispatching Scalar/Sse2/Avx2 per entry — a future AVX-512 tier
    is a new SimdTier value + TU, no interface change. Device backends
    register whole TensorOps tables instead (the DeviceBackend plugin
    contract); they never see this table.

    Numeric contracts shared by every backend (the scalar lanes are the
    authority): floats follow IEEE (div by zero yields inf/nan, no
    raise — only the language's int div raises); unary/binary lane math
    matches the scalar lanes within the VecMath tolerance contract;
    max/min keep the scalar walk's exact order rule (a NaN cell never
    wins unless it was the first element; ties keep the earlier
    element), with one carve-out: +0.0 and -0.0 compare equal under
    the strict compares both paths use, and on a signed-zero tie the
    vectorized fold's lane structure can surface the other zero's bit
    pattern than the scalar walk's first-seen pick (verified: max over
    [-1, -1, +0.0, -1, -1, -0.0, ...] reads back "-0" on sse2/avx2 and
    "0" on scalar). The sign of a zero result is tier-dependent — the
    language cannot spell a preference. Only float/double ride the
    table — int kernels keep the language's wrap-and-raise semantics
    in their scalar loops.
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
