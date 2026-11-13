/*---
    VecBinaryAvx2.cpp — AVX2 elementwise lanes: binary, scalar-binary
    and clamp over flat spans (x64 only; /arch:AVX2 TU). Scalar tails
    run the shared carrier lanes from Elementwise.h, so the fallback
    and the vector path can never drift apart semantically.
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include "Elementwise.h"

#include <immintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {
namespace {

template <typename T>
struct AvxLanes;

template <>
struct AvxLanes<float> {
    using V = __m256;
    static constexpr int64_t kLanes = 8;
    static V Load(const float* p) { return _mm256_loadu_ps(p); }
    static void Store(float* p, V v) { _mm256_storeu_ps(p, v); }
    static V Set1(float s) { return _mm256_set1_ps(s); }
    static V Binary(BinaryKernelOp op, V a, V b) {
        switch (op) {
            case BinaryKernelOp::Add: return _mm256_add_ps(a, b);
            case BinaryKernelOp::Sub: return _mm256_sub_ps(a, b);
            case BinaryKernelOp::Mul: return _mm256_mul_ps(a, b);
            case BinaryKernelOp::Div: return _mm256_div_ps(a, b);
            case BinaryKernelOp::Rsub: return _mm256_sub_ps(b, a);
            case BinaryKernelOp::Rdiv: return _mm256_div_ps(b, a);
            case BinaryKernelOp::ReLUGrad: {
                //b > 0 ? a : +0 — the all-ones mask bit-preserves a.
                const V mask = _mm256_cmp_ps(b, _mm256_setzero_ps(),
                                             _CMP_GT_OQ);
                return _mm256_and_ps(a, mask);
            }
        }
        return a;
    }
    static V Clamp(bool isMax, V cur, V bound) {
        //max/min with (cur, bound) order reproduce the scalar rule
        //exactly: ties and NaNs resolve to the bound (second operand).
        return isMax ? _mm256_max_ps(cur, bound) : _mm256_min_ps(cur, bound);
    }
};

template <>
struct AvxLanes<double> {
    using V = __m256d;
    static constexpr int64_t kLanes = 4;
    static V Load(const double* p) { return _mm256_loadu_pd(p); }
    static void Store(double* p, V v) { _mm256_storeu_pd(p, v); }
    static V Set1(double s) { return _mm256_set1_pd(s); }
    static V Binary(BinaryKernelOp op, V a, V b) {
        switch (op) {
            case BinaryKernelOp::Add: return _mm256_add_pd(a, b);
            case BinaryKernelOp::Sub: return _mm256_sub_pd(a, b);
            case BinaryKernelOp::Mul: return _mm256_mul_pd(a, b);
            case BinaryKernelOp::Div: return _mm256_div_pd(a, b);
            case BinaryKernelOp::Rsub: return _mm256_sub_pd(b, a);
            case BinaryKernelOp::Rdiv: return _mm256_div_pd(b, a);
            case BinaryKernelOp::ReLUGrad: {
                const V mask = _mm256_cmp_pd(b, _mm256_setzero_pd(),
                                             _CMP_GT_OQ);
                return _mm256_and_pd(a, mask);
            }
        }
        return a;
    }
    static V Clamp(bool isMax, V cur, V bound) {
        return isMax ? _mm256_max_pd(cur, bound) : _mm256_min_pd(cur, bound);
    }
};

template <typename T>
void FlatBinaryAvx2(BinaryKernelOp op, const T* a, const T* b, T* out,
                    int64_t count) {
    using Tr = AvxLanes<T>;
    int64_t i = 0;
    for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
        Tr::Store(out + i, Tr::Binary(op, Tr::Load(a + i), Tr::Load(b + i)));
    }
    for (; i < count; ++i) {
        out[i] = CarrierBinary<T>(op, a[i], b[i]);
    }
}

template <typename T>
void FlatBinaryScalarAvx2(BinaryKernelOp op, const T* a, T scalar, T* out,
                          int64_t count) {
    using Tr = AvxLanes<T>;
    const typename Tr::V bv = Tr::Set1(scalar);
    int64_t i = 0;
    for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
        Tr::Store(out + i, Tr::Binary(op, Tr::Load(a + i), bv));
    }
    for (; i < count; ++i) {
        out[i] = CarrierBinary<T>(op, a[i], scalar);
    }
}

template <typename T>
void FlatClampAvx2(bool isMax, const T* src, T bound, T* out,
                   int64_t count) {
    using Tr = AvxLanes<T>;
    const typename Tr::V bv = Tr::Set1(bound);
    int64_t i = 0;
    for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
        Tr::Store(out + i, Tr::Clamp(isMax, Tr::Load(src + i), bv));
    }
    for (; i < count; ++i) {
        out[i] = isMax ? (src[i] > bound ? src[i] : bound)
                       : (src[i] < bound ? src[i] : bound);
    }
}

} // namespace

void BinaryF32Avx2(BinaryKernelOp op, const float* a, const float* b,
                   float* out, int64_t count) {
    FlatBinaryAvx2(op, a, b, out, count);
}

void BinaryF64Avx2(BinaryKernelOp op, const double* a, const double* b,
                   double* out, int64_t count) {
    FlatBinaryAvx2(op, a, b, out, count);
}

void BinaryScalarF32Avx2(BinaryKernelOp op, const float* a, float scalar,
                         float* out, int64_t count) {
    FlatBinaryScalarAvx2(op, a, scalar, out, count);
}

void BinaryScalarF64Avx2(BinaryKernelOp op, const double* a, double scalar,
                         double* out, int64_t count) {
    FlatBinaryScalarAvx2(op, a, scalar, out, count);
}

void ClampF32Avx2(bool isMax, const float* src, float bound, float* out,
                  int64_t count) {
    FlatClampAvx2(isMax, src, bound, out, count);
}

void ClampF64Avx2(bool isMax, const double* src, double bound, double* out,
                  int64_t count) {
    FlatClampAvx2(isMax, src, bound, out, count);
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
