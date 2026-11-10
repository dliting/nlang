/*---
    MatMulAvx2.cpp — AVX2+FMA matmul lanes (x64 only; this TU compiles
    with /arch:AVX2 — vector types never leave the file).

    Micro-kernel shape: 4 rows of A x 8 lanes of B (one __m256
    accumulator per A row). The k loop keeps its scalar i-k-j order per
    output element (broadcast a[i,k], FMA against the loaded B row), so
    float results differ from the scalar loop only by FMA's missing
    intermediate rounding — the tolerance the tests pin. Row/column
    tails run the same order scalar. B is streamed row-major; at MNIST
    workloads B fits L2 and the A broadcasts stay in registers, which
    is the whole budget story for this iteration (no packing).
---*/
#include "MatMulTiers.h"

#if defined(NLANG_SIMD_X64)
#include <immintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {

void MatMulF32Avx2(int32_t rows, int32_t inner, int32_t cols,
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
        for (; j + 8 <= cols; j += 8) {
            __m256 c0 = _mm256_setzero_ps();
            __m256 c1 = _mm256_setzero_ps();
            __m256 c2 = _mm256_setzero_ps();
            __m256 c3 = _mm256_setzero_ps();
            for (int32_t k = 0; k < inner; ++k) {
                const __m256 bv = _mm256_loadu_ps(b + static_cast<int64_t>(k) * n64 + j);
                c0 = _mm256_fmadd_ps(_mm256_broadcast_ss(aRow[0] + k), bv, c0);
                c1 = _mm256_fmadd_ps(_mm256_broadcast_ss(aRow[1] + k), bv, c1);
                c2 = _mm256_fmadd_ps(_mm256_broadcast_ss(aRow[2] + k), bv, c2);
                c3 = _mm256_fmadd_ps(_mm256_broadcast_ss(aRow[3] + k), bv, c3);
            }
            _mm256_storeu_ps(oRow[0] + j, c0);
            _mm256_storeu_ps(oRow[1] + j, c1);
            _mm256_storeu_ps(oRow[2] + j, c2);
            _mm256_storeu_ps(oRow[3] + j, c3);
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
        for (; j + 8 <= cols; j += 8) {
            __m256 c0 = _mm256_setzero_ps();
            for (int32_t k = 0; k < inner; ++k) {
                c0 = _mm256_fmadd_ps(_mm256_broadcast_ss(aRow + k),
                    _mm256_loadu_ps(b + static_cast<int64_t>(k) * n64 + j),
                    c0);
            }
            _mm256_storeu_ps(oRow + j, c0);
        }
        for (; j < cols; ++j) {
            float s0 = 0.0f;
            for (int32_t k = 0; k < inner; ++k)
                s0 += aRow[k] * b[static_cast<int64_t>(k) * n64 + j];
            oRow[j] = s0;
        }
    }
}

void MatMulF64Avx2(int32_t rows, int32_t inner, int32_t cols,
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
        for (; j + 4 <= cols; j += 4) {
            __m256d c0 = _mm256_setzero_pd();
            __m256d c1 = _mm256_setzero_pd();
            __m256d c2 = _mm256_setzero_pd();
            __m256d c3 = _mm256_setzero_pd();
            for (int32_t k = 0; k < inner; ++k) {
                const __m256d bv = _mm256_loadu_pd(b + static_cast<int64_t>(k) * n64 + j);
                c0 = _mm256_fmadd_pd(_mm256_broadcast_sd(aRow[0] + k), bv, c0);
                c1 = _mm256_fmadd_pd(_mm256_broadcast_sd(aRow[1] + k), bv, c1);
                c2 = _mm256_fmadd_pd(_mm256_broadcast_sd(aRow[2] + k), bv, c2);
                c3 = _mm256_fmadd_pd(_mm256_broadcast_sd(aRow[3] + k), bv, c3);
            }
            _mm256_storeu_pd(oRow[0] + j, c0);
            _mm256_storeu_pd(oRow[1] + j, c1);
            _mm256_storeu_pd(oRow[2] + j, c2);
            _mm256_storeu_pd(oRow[3] + j, c3);
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
        for (; j + 4 <= cols; j += 4) {
            __m256d c0 = _mm256_setzero_pd();
            for (int32_t k = 0; k < inner; ++k) {
                c0 = _mm256_fmadd_pd(_mm256_broadcast_sd(aRow + k),
                    _mm256_loadu_pd(b + static_cast<int64_t>(k) * n64 + j),
                    c0);
            }
            _mm256_storeu_pd(oRow + j, c0);
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
