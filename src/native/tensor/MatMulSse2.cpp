/*---
    MatMulSse2.cpp — SSE2 matmul lanes (x64 baseline, no per-file flag).
    4 rows x 4 lanes tiles; SSE2 has no FMA so the inner step is
    mulps + addps — same scalar k accumulation order as the reference.
---*/
#include "MatMulTiers.h"

#if defined(NLANG_SIMD_X64)
#include <emmintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {

void MatMulF32Sse2(int32_t rows, int32_t inner, int32_t cols,
                   const float* a, const float* b, float* out) {
    const int64_t n64 = cols;
    int32_t i = 0;
    for (; i + 4 <= rows; i += 4) {
        const float* aRow[4];
        float* oRow[4];
        for (int32_t r = 0; r < 4; ++r) {
            aRow[r] = a + static_cast<int64_t>(i + r) * inner;
            oRow[r] = out + static_cast<int64_t>(i + r) * n64;
        }
        int32_t j = 0;
        for (; j + 4 <= cols; j += 4) {
            __m128 c0 = _mm_setzero_ps();
            __m128 c1 = _mm_setzero_ps();
            __m128 c2 = _mm_setzero_ps();
            __m128 c3 = _mm_setzero_ps();
            for (int32_t k = 0; k < inner; ++k) {
                const __m128 bv = _mm_loadu_ps(b + static_cast<int64_t>(k) * n64 + j);
                c0 = _mm_add_ps(c0, _mm_mul_ps(_mm_set1_ps(aRow[0][k]), bv));
                c1 = _mm_add_ps(c1, _mm_mul_ps(_mm_set1_ps(aRow[1][k]), bv));
                c2 = _mm_add_ps(c2, _mm_mul_ps(_mm_set1_ps(aRow[2][k]), bv));
                c3 = _mm_add_ps(c3, _mm_mul_ps(_mm_set1_ps(aRow[3][k]), bv));
            }
            _mm_storeu_ps(oRow[0] + j, c0);
            _mm_storeu_ps(oRow[1] + j, c1);
            _mm_storeu_ps(oRow[2] + j, c2);
            _mm_storeu_ps(oRow[3] + j, c3);
        }
        for (; j < cols; ++j) {
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
            for (int32_t k = 0; k < inner; ++k) {
                const float bv = b[static_cast<int64_t>(k) * n64 + j];
                s0 += aRow[0][k] * bv;
                s1 += aRow[1][k] * bv;
                s2 += aRow[2][k] * bv;
                s3 += aRow[3][k] * bv;
            }
            oRow[0][j] = s0;
            oRow[1][j] = s1;
            oRow[2][j] = s2;
            oRow[3][j] = s3;
        }
    }
    for (; i < rows; ++i) {
        const float* aRow = a + static_cast<int64_t>(i) * inner;
        float* oRow = out + static_cast<int64_t>(i) * n64;
        int32_t j = 0;
        for (; j + 4 <= cols; j += 4) {
            __m128 c0 = _mm_setzero_ps();
            for (int32_t k = 0; k < inner; ++k) {
                c0 = _mm_add_ps(c0, _mm_mul_ps(_mm_set1_ps(aRow[k]),
                    _mm_loadu_ps(b + static_cast<int64_t>(k) * n64 + j)));
            }
            _mm_storeu_ps(oRow + j, c0);
        }
        for (; j < cols; ++j) {
            float s0 = 0.0f;
            for (int32_t k = 0; k < inner; ++k)
                s0 += aRow[k] * b[static_cast<int64_t>(k) * n64 + j];
            oRow[j] = s0;
        }
    }
}

void MatMulF64Sse2(int32_t rows, int32_t inner, int32_t cols,
                   const double* a, const double* b, double* out) {
    const int64_t n64 = cols;
    int32_t i = 0;
    for (; i + 4 <= rows; i += 4) {
        const double* aRow[4];
        double* oRow[4];
        for (int32_t r = 0; r < 4; ++r) {
            aRow[r] = a + static_cast<int64_t>(i + r) * inner;
            oRow[r] = out + static_cast<int64_t>(i + r) * n64;
        }
        int32_t j = 0;
        for (; j + 2 <= cols; j += 2) {
            __m128d c0 = _mm_setzero_pd();
            __m128d c1 = _mm_setzero_pd();
            __m128d c2 = _mm_setzero_pd();
            __m128d c3 = _mm_setzero_pd();
            for (int32_t k = 0; k < inner; ++k) {
                const __m128d bv = _mm_loadu_pd(b + static_cast<int64_t>(k) * n64 + j);
                c0 = _mm_add_pd(c0, _mm_mul_pd(_mm_set1_pd(aRow[0][k]), bv));
                c1 = _mm_add_pd(c1, _mm_mul_pd(_mm_set1_pd(aRow[1][k]), bv));
                c2 = _mm_add_pd(c2, _mm_mul_pd(_mm_set1_pd(aRow[2][k]), bv));
                c3 = _mm_add_pd(c3, _mm_mul_pd(_mm_set1_pd(aRow[3][k]), bv));
            }
            _mm_storeu_pd(oRow[0] + j, c0);
            _mm_storeu_pd(oRow[1] + j, c1);
            _mm_storeu_pd(oRow[2] + j, c2);
            _mm_storeu_pd(oRow[3] + j, c3);
        }
        for (; j < cols; ++j) {
            double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
            for (int32_t k = 0; k < inner; ++k) {
                const double bv = b[static_cast<int64_t>(k) * n64 + j];
                s0 += aRow[0][k] * bv;
                s1 += aRow[1][k] * bv;
                s2 += aRow[2][k] * bv;
                s3 += aRow[3][k] * bv;
            }
            oRow[0][j] = s0;
            oRow[1][j] = s1;
            oRow[2][j] = s2;
            oRow[3][j] = s3;
        }
    }
    for (; i < rows; ++i) {
        const double* aRow = a + static_cast<int64_t>(i) * inner;
        double* oRow = out + static_cast<int64_t>(i) * n64;
        int32_t j = 0;
        for (; j + 2 <= cols; j += 2) {
            __m128d c0 = _mm_setzero_pd();
            for (int32_t k = 0; k < inner; ++k) {
                c0 = _mm_add_pd(c0, _mm_mul_pd(_mm_set1_pd(aRow[k]),
                    _mm_loadu_pd(b + static_cast<int64_t>(k) * n64 + j)));
            }
            _mm_storeu_pd(oRow + j, c0);
        }
        for (; j < cols; ++j) {
            double s0 = 0.0;
            for (int32_t k = 0; k < inner; ++k)
                s0 += aRow[k] * b[static_cast<int64_t>(k) * n64 + j];
            oRow[j] = s0;
        }
    }
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
