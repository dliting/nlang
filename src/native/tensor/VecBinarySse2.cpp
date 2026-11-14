/*---
    VecBinarySse2.cpp — SSE2 elementwise lanes (x64 baseline): binary,
    scalar-binary and clamp over flat spans, 4-wide. Same shape as the
    AVX2 TU; scalar tails share the carrier lanes from Elementwise.h.
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include "Elementwise.h"

#include <emmintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {
namespace {

template <typename T>
struct SseLanes;

template <>
struct SseLanes<float> {
    using V = __m128;
    static constexpr int64_t kLanes = 4;
    static V Load(const float* p) { return _mm_loadu_ps(p); }
    static void Store(float* p, V v) { _mm_storeu_ps(p, v); }
    static V Set1(float s) { return _mm_set1_ps(s); }
    static V Binary(BinaryKernelOp op, V a, V b) {
        switch (op) {
            case BinaryKernelOp::Add: return _mm_add_ps(a, b);
            case BinaryKernelOp::Sub: return _mm_sub_ps(a, b);
            case BinaryKernelOp::Mul: return _mm_mul_ps(a, b);
            case BinaryKernelOp::Div: return _mm_div_ps(a, b);
            case BinaryKernelOp::Rsub: return _mm_sub_ps(b, a);
            case BinaryKernelOp::Rdiv: return _mm_div_ps(b, a);
            case BinaryKernelOp::ReLUGrad: {
                const V mask = _mm_cmpgt_ps(b, _mm_setzero_ps());
                return _mm_and_ps(a, mask);
            }
        }
        return a;
    }
    static V Clamp(bool isMax, V cur, V bound) {
        return isMax ? _mm_max_ps(cur, bound) : _mm_min_ps(cur, bound);
    }
};

template <>
struct SseLanes<double> {
    using V = __m128d;
    static constexpr int64_t kLanes = 2;
    static V Load(const double* p) { return _mm_loadu_pd(p); }
    static void Store(double* p, V v) { _mm_storeu_pd(p, v); }
    static V Set1(double s) { return _mm_set1_pd(s); }
    static V Binary(BinaryKernelOp op, V a, V b) {
        switch (op) {
            case BinaryKernelOp::Add: return _mm_add_pd(a, b);
            case BinaryKernelOp::Sub: return _mm_sub_pd(a, b);
            case BinaryKernelOp::Mul: return _mm_mul_pd(a, b);
            case BinaryKernelOp::Div: return _mm_div_pd(a, b);
            case BinaryKernelOp::Rsub: return _mm_sub_pd(b, a);
            case BinaryKernelOp::Rdiv: return _mm_div_pd(b, a);
            case BinaryKernelOp::ReLUGrad: {
                const V mask = _mm_cmpgt_pd(b, _mm_setzero_pd());
                return _mm_and_pd(a, mask);
            }
        }
        return a;
    }
    static V Clamp(bool isMax, V cur, V bound) {
        return isMax ? _mm_max_pd(cur, bound) : _mm_min_pd(cur, bound);
    }
};

template <typename T>
void FlatBinarySse2(BinaryKernelOp op, const T* a, const T* b, T* out,
                    int64_t count) {
    using Tr = SseLanes<T>;
    int64_t i = 0;
    for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
        Tr::Store(out + i, Tr::Binary(op, Tr::Load(a + i), Tr::Load(b + i)));
    }
    for (; i < count; ++i) {
        out[i] = CarrierBinary<T>(op, a[i], b[i]);
    }
}

template <typename T>
void FlatBinaryScalarSse2(BinaryKernelOp op, const T* a, T scalar, T* out,
                          int64_t count) {
    using Tr = SseLanes<T>;
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
void FlatClampSse2(bool isMax, const T* src, T bound, T* out,
                   int64_t count) {
    using Tr = SseLanes<T>;
    const typename Tr::V bv = Tr::Set1(bound);
    int64_t i = 0;
    for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
        Tr::Store(out + i, Tr::Clamp(isMax, Tr::Load(src + i), bv));
    }
    for (; i < count; ++i) {
        out[i] = ClampCell(isMax, src[i], bound);
    }
}

} // namespace

void BinaryF32Sse2(BinaryKernelOp op, const float* a, const float* b,
                   float* out, int64_t count) {
    FlatBinarySse2(op, a, b, out, count);
}

void BinaryF64Sse2(BinaryKernelOp op, const double* a, const double* b,
                   double* out, int64_t count) {
    FlatBinarySse2(op, a, b, out, count);
}

void BinaryScalarF32Sse2(BinaryKernelOp op, const float* a, float scalar,
                         float* out, int64_t count) {
    FlatBinaryScalarSse2(op, a, scalar, out, count);
}

void BinaryScalarF64Sse2(BinaryKernelOp op, const double* a, double scalar,
                         double* out, int64_t count) {
    FlatBinaryScalarSse2(op, a, scalar, out, count);
}

void ClampF32Sse2(bool isMax, const float* src, float bound, float* out,
                  int64_t count) {
    FlatClampSse2(isMax, src, bound, out, count);
}

void ClampF64Sse2(bool isMax, const double* src, double bound, double* out,
                  int64_t count) {
    FlatClampSse2(isMax, src, bound, out, count);
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
