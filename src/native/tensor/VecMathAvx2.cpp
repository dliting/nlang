/*---
    VecMathAvx2.cpp — AVX2+FMA tier lanes of the vector math layer
    (x64 only; this TU is compiled with /arch:AVX2 — see the CMake
    mixed-arch discipline: vector types stay inside this file). Same
    reduction-and-Taylor algorithm as VecMathSse2.cpp, 256 bits wide —
    the full derivation and constant budget live in that file's header
    comment.
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include <immintrin.h>

#include <cstdint>
#include <limits>

namespace tensor {
namespace detail {
namespace {

constexpr float kLog2e = 1.4426950408889634f;
constexpr float kLn2Hi = 0.693115234375f;
constexpr float kLn2Lo = 3.1946186e-05f;
constexpr float kMaxInput = 88.72284f;
constexpr float kMinInput = -103.972f;
constexpr float kMinReduction = -127.49f;
constexpr float kTanPolyEdge = 0.5f;

constexpr float kInv2 = 0.5f;
constexpr float kInv6 = 1.0f / 6.0f;
constexpr float kInv24 = 1.0f / 24.0f;
constexpr float kInv120 = 1.0f / 120.0f;
constexpr float kInv720 = 1.0f / 720.0f;
constexpr float kInv5040 = 1.0f / 5040.0f;

constexpr float kTanQ6 = 21844.0f / 6081075.0f;
constexpr float kTanQ5 = -1382.0f / 155925.0f;
constexpr float kTanQ4 = 62.0f / 2835.0f;
constexpr float kTanQ3 = -17.0f / 315.0f;
constexpr float kTanQ2 = 2.0f / 15.0f;
constexpr float kTanQ1 = -1.0f / 3.0f;

__m256 ExpF32Lanes(__m256 x) {
    const __m256 nanMask = _mm256_cmp_ps(x, x, _CMP_UNORD_Q);
    const __m256 overMask = _mm256_cmp_ps(x, _mm256_set1_ps(kMaxInput),
                                          _CMP_GT_OQ);
    const __m256 underMask = _mm256_cmp_ps(x, _mm256_set1_ps(kMinInput),
                                           _CMP_LT_OQ);
    __m256 xc = _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(kMinInput)),
                              _mm256_set1_ps(kMaxInput));
    __m256 r = _mm256_mul_ps(xc, _mm256_set1_ps(kLog2e));
    r = _mm256_max_ps(r, _mm256_set1_ps(kMinReduction));
    const __m256i nInt = _mm256_cvtps_epi32(r);
    const __m256 n = _mm256_cvtepi32_ps(nInt);
    __m256 f = _mm256_sub_ps(xc, _mm256_mul_ps(n,
        _mm256_set1_ps(kLn2Hi)));
    f = _mm256_sub_ps(f, _mm256_mul_ps(n, _mm256_set1_ps(kLn2Lo)));
    __m256 p = _mm256_set1_ps(kInv5040);
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(kInv720));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(kInv120));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(kInv24));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(kInv6));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(kInv2));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));
    const __m256i nBits = _mm256_min_epi32(nInt, _mm256_set1_epi32(127));
    const __m256i bits = _mm256_slli_epi32(_mm256_add_epi32(nBits,
        _mm256_set1_epi32(127)), 23);
    __m256 res = _mm256_mul_ps(p, _mm256_castsi256_ps(bits));
    const __m256i over128 = _mm256_cmpgt_epi32(nInt,
        _mm256_set1_epi32(127));
    const __m256 scale = _mm256_or_ps(
        _mm256_and_ps(_mm256_castsi256_ps(over128),
                      _mm256_set1_ps(2.0f)),
        _mm256_andnot_ps(_mm256_castsi256_ps(over128),
                         _mm256_set1_ps(1.0f)));
    res = _mm256_mul_ps(res, scale);
    res = _mm256_andnot_ps(underMask, res);
    res = _mm256_or_ps(_mm256_andnot_ps(overMask, res),
        _mm256_and_ps(overMask,
            _mm256_set1_ps(std::numeric_limits<float>::infinity())));
    return _mm256_or_ps(_mm256_andnot_ps(nanMask, res),
                        _mm256_and_ps(nanMask, x));
}

__m256 TanhF32Lanes(__m256 x) {
    //Large-|x| branch: the exp form saturates at +-1 by itself (the
    //exp lane's overflow to +inf feeds 2/inf = 0) and NaN propagates
    //through it; small-|x| branch: the odd Taylor.
    const __m256 twoX = _mm256_add_ps(x, x);
    const __m256 e = ExpF32Lanes(twoX);
    const __m256 tExp = _mm256_sub_ps(_mm256_set1_ps(1.0f),
        _mm256_div_ps(_mm256_set1_ps(2.0f),
            _mm256_add_ps(e, _mm256_set1_ps(1.0f))));
    const __m256 x2 = _mm256_mul_ps(x, x);
    __m256 q = _mm256_set1_ps(kTanQ6);
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(kTanQ5));
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(kTanQ4));
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(kTanQ3));
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(kTanQ2));
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(kTanQ1));
    q = _mm256_fmadd_ps(q, x2, _mm256_set1_ps(1.0f));
    const __m256 tPoly = _mm256_mul_ps(x, q);
    const __m256 small = _mm256_cmp_ps(
        _mm256_and_ps(x,
            _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF))),
        _mm256_set1_ps(kTanPolyEdge), _CMP_LT_OQ); //|x| via the sign-bit mask
    return _mm256_blendv_ps(tExp, tPoly, small);
}

} // namespace

void ExpF32Avx2(const float* src, float* dst, int64_t count) {
    int64_t i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_ps(dst + i,
                         ExpF32Lanes(_mm256_loadu_ps(src + i)));
    }
    for (; i < count; ++i) {
        dst[i] = _mm256_cvtss_f32(ExpF32Lanes(_mm256_set1_ps(src[i])));
    }
}

void TanhF32Avx2(const float* src, float* dst, int64_t count) {
    int64_t i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_ps(dst + i,
                         TanhF32Lanes(_mm256_loadu_ps(src + i)));
    }
    for (; i < count; ++i) {
        dst[i] = _mm256_cvtss_f32(TanhF32Lanes(_mm256_set1_ps(src[i])));
    }
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
