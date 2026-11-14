/*---
    KernelOpsCpu.cpp — the built-in CPU backend: the KernelOps table
    entries dispatch the SIMD tier per call (scalar lanes live here as
    the portable reference; the x64 tier lanes sit in the tier TUs).
    This is the only backend in this iteration; device backends
    register their own tables (see TensorBackends.h).
---*/
#include "TensorBackends.h"

#include "Elementwise.h"
#include "SimdCpu.h"

#if defined(NLANG_SIMD_X64)
#include "MatMulTiers.h"
#include "VecMathTiers.h"
#endif

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace tensor {

namespace {

//--- scalar lanes (the portable reference; the tier TUs must match
//    these within the pinned tolerances) ---

double ScalarUnary(UnaryKernelOp op, double v) {
    switch (op) {
        case UnaryKernelOp::Exp: return std::exp(v);
        case UnaryKernelOp::Log: return std::log(v);
        case UnaryKernelOp::Tanh: return std::tanh(v);
        case UnaryKernelOp::Neg: return -v;
    }
    return 0.0;
}

template <typename T>
void CpuUnaryScalarLoop(UnaryKernelOp op, const T* src, T* dst,
                        int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
        dst[i] = static_cast<T>(ScalarUnary(op,
            static_cast<double>(src[i])));
    }
}

//Only exp/tanh have tier lanes this iteration; log/neg run the scalar
//loop at every tier (never the bottleneck; a lane slot is all a future
//backend entry needs).
template <typename T>
void CpuUnary(UnaryKernelOp op, const T* src, T* dst, int64_t count) {
#if defined(NLANG_SIMD_X64)
    if ((op == UnaryKernelOp::Exp || op == UnaryKernelOp::Tanh)
        && ActiveSimdTier() != SimdTier::Scalar) {
        const bool wantAvx2 = ActiveSimdTier() == SimdTier::Avx2;
        if constexpr (std::is_same_v<T, float>) {
            if (op == UnaryKernelOp::Exp) {
                wantAvx2 ? detail::ExpF32Avx2(src, dst, count)
                         : detail::ExpF32Sse2(src, dst, count);
            } else {
                wantAvx2 ? detail::TanhF32Avx2(src, dst, count)
                         : detail::TanhF32Sse2(src, dst, count);
            }
            return;
        } else {
            //f64 unary: the scalar loop is the lane today (spec D6).
            (void)wantAvx2;
        }
    }
#endif
    CpuUnaryScalarLoop(op, src, dst, count);
}

template <typename T>
void CpuBinaryScalarLoop(BinaryKernelOp op, const T* a, const T* b,
                         T* out, int64_t count) {
    for (int64_t i = 0; i < count; ++i) {
        out[i] = detail::CarrierBinary<T>(op, a[i], b[i]);
    }
}

template <typename T>
void CpuBinary(BinaryKernelOp op, const T* a, const T* b, T* out,
               int64_t count) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::BinaryF32Avx2(op, a, b, out, count);
            return;
        }
        detail::BinaryF32Sse2(op, a, b, out, count);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::BinaryF64Avx2(op, a, b, out, count);
            return;
        }
        detail::BinaryF64Sse2(op, a, b, out, count);
        return;
    }
#endif
    CpuBinaryScalarLoop(op, a, b, out, count);
}

template <typename T>
void CpuBinaryScalar(BinaryKernelOp op, const T* a, T scalar, T* out,
                     int64_t count) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::BinaryScalarF32Avx2(op, a, scalar, out, count);
            return;
        }
        detail::BinaryScalarF32Sse2(op, a, scalar, out, count);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::BinaryScalarF64Avx2(op, a, scalar, out, count);
            return;
        }
        detail::BinaryScalarF64Sse2(op, a, scalar, out, count);
        return;
    }
#endif
    for (int64_t i = 0; i < count; ++i) {
        out[i] = detail::CarrierBinary<T>(op, a[i], scalar);
    }
}

template <typename T>
void CpuClamp(bool isMax, const T* src, T bound, T* out, int64_t count) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::ClampF32Avx2(isMax, src, bound, out, count);
            return;
        }
        detail::ClampF32Sse2(isMax, src, bound, out, count);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::ClampF64Avx2(isMax, src, bound, out, count);
            return;
        }
        detail::ClampF64Sse2(isMax, src, bound, out, count);
        return;
    }
#endif
    for (int64_t i = 0; i < count; ++i) {
        out[i] = detail::ClampCell(isMax, src[i], bound);
    }
}

template <typename T>
void CpuSumScalarLoop(const T* cells, int64_t count, double* out) {
    double acc = 0.0;
    for (int64_t i = 0; i < count; ++i) {
        acc += cells[i];
    }
    *out = acc;
}

template <typename T>
void CpuSum(const T* cells, int64_t count, double* out) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::SumF32Avx2(cells, count, out);
            return;
        }
        detail::SumF32Sse2(cells, count, out);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::SumF64Avx2(cells, count, out);
            return;
        }
        detail::SumF64Sse2(cells, count, out);
        return;
    }
#endif
    CpuSumScalarLoop(cells, count, out);
}

template <typename T>
void CpuDotScalarLoop(const T* a, const T* b, int64_t count, T* out) {
    T acc = T(0);
    for (int64_t i = 0; i < count; ++i) {
        acc = static_cast<T>(acc + a[i] * b[i]);
    }
    *out = acc;
}

template <typename T>
void CpuDot(const T* a, const T* b, int64_t count, T* out) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::DotF32Avx2(a, b, count, out);
            return;
        }
        detail::DotF32Sse2(a, b, count, out);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::DotF64Avx2(a, b, count, out);
            return;
        }
        detail::DotF64Sse2(a, b, count, out);
        return;
    }
#endif
    CpuDotScalarLoop(a, b, count, out);
}

//Extreme with the scalar walk's NaN/tie rule as the reference loop.
template <typename T>
void CpuExtremeScalarLoop(bool isMax, const T* cells, int64_t count,
                          T* out) {
    T best = cells[0];
    for (int64_t i = 1; i < count; ++i) {
        const T cur = cells[i];
        if (isMax ? cur > best : cur < best) {
            best = cur;
        }
    }
    *out = best;
}

template <typename T>
void CpuExtreme(bool isMax, const T* cells, int64_t count, T* out) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            if (isMax) {
                detail::MaxF32Avx2(cells, count, out);
            } else {
                detail::MinF32Avx2(cells, count, out);
            }
            return;
        }
        if (isMax) {
            detail::MaxF32Sse2(cells, count, out);
        } else {
            detail::MinF32Sse2(cells, count, out);
        }
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            if (isMax) {
                detail::MaxF64Avx2(cells, count, out);
            } else {
                detail::MinF64Avx2(cells, count, out);
            }
            return;
        }
        if (isMax) {
            detail::MaxF64Sse2(cells, count, out);
        } else {
            detail::MinF64Sse2(cells, count, out);
        }
        return;
    }
#endif
    CpuExtremeScalarLoop(isMax, cells, count, out);
}

template <typename T>
void CpuAddIntoF64(double* acc, const T* row, int64_t count) {
#if defined(NLANG_SIMD_X64)
    if (ActiveSimdTier() == SimdTier::Scalar) {
        //fall through to the portable scalar loop
    } else     if constexpr (std::is_same_v<T, float>) {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::AddIntoF64FromF32Avx2(acc, row, count);
            return;
        }
        detail::AddIntoF64FromF32Sse2(acc, row, count);
        return;
    } else {
        if (ActiveSimdTier() == SimdTier::Avx2) {
            detail::AddIntoF64Avx2(acc, row, count);
            return;
        }
        detail::AddIntoF64Sse2(acc, row, count);
        return;
    }
#endif
    for (int64_t i = 0; i < count; ++i) {
        acc[i] += row[i];
    }
}

//--- scalar matmul (the portable reference and the fallback tier):
//    plain i-k-j — the exact accumulation order the vector kernels
//    preserve.

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

//The table exposes max/min as separate slots; the tier machinery is
//shared behind CpuExtreme.
void CpuMaxF32(const float* cells, int64_t count, float* out) {
    CpuExtreme(true, cells, count, out);
}

void CpuMaxF64(const double* cells, int64_t count, double* out) {
    CpuExtreme(true, cells, count, out);
}

void CpuMinF32(const float* cells, int64_t count, float* out) {
    CpuExtreme(false, cells, count, out);
}

void CpuMinF64(const double* cells, int64_t count, double* out) {
    CpuExtreme(false, cells, count, out);
}

void CpuAddIntoF64FromF32(double* acc, const float* row, int64_t count) {
    CpuAddIntoF64<float>(acc, row, count);
}

const KernelOps kCpuKernelOps = {
    &CpuUnary<float>,
    &CpuUnary<double>,
    &CpuBinary<float>,
    &CpuBinary<double>,
    &CpuBinaryScalar<float>,
    &CpuBinaryScalar<double>,
    &CpuClamp<float>,
    &CpuClamp<double>,
    &CpuSum<float>,
    &CpuSum<double>,
    &CpuDot<float>,
    &CpuDot<double>,
    &CpuMaxF32,
    &CpuMaxF64,
    &CpuMinF32,
    &CpuMinF64,
    &CpuAddIntoF64FromF32,
    &CpuAddIntoF64<double>,
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
