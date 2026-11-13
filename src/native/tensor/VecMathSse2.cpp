/*---
    VecMathSse2.cpp — SSE2 tier lanes of the vector math layer (x64
    only; SSE2 is the architectural baseline so no per-file arch flag).
    Same algorithm as the AVX2 TU, 128 bits wide:

    exp(x): n = round(x*log2e); f = (x - n*c1) - n*c2 with c1+c2 = ln2
    and c1 a 12-bit mantissa constant (Cody-Waite — n*c1 is exact in
    f32, so f carries only the poly rounding, not the log2e product
    rounding); exp(f) by a degree-7 Taylor (truncation < 1e-8 rel for
    |f| <= ln2/2); 2^n assembled from the exponent bits, n clamped to
    127 for the assembly and the representable-edge n==128 restored by
    a 2x scale. IEEE edges are explicit masks: NaN in -> NaN (itself),
    x > 88.72284 (ln FLT_MAX) -> +inf, x < -103.972 -> +0; the low-side
    r clamp keeps the int conversion out of garbage territory for
    everything the mask has not already zeroed.

    tanh(x): 1 - 2/(1 + exp(2x)) for |x| >= 0.625 (saturates by
    itself; no cancellation at that scale), a degree-11 odd Taylor for
    |x| < 0.625 (the exp form's error grows like 1/|x| near zero).
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include <emmintrin.h>

#include <cstdint>
#include <limits>

namespace tensor {
namespace detail {
namespace {

constexpr float kLog2e = 1.4426950408889634f;
//Cody-Waite split of ln2: c1 keeps 12 mantissa bits so n*c1 (n <= 255)
//is exact in f32; c2 = ln2 - c1 covers the rest.
constexpr float kLn2Hi = 0.693115234375f;
constexpr float kLn2Lo = 3.1946186e-05f;
constexpr float kMaxInput = 88.72284f;
constexpr float kMinInput = -103.972f;
//Keeps (n+127) >= 0 in the exponent assembly; the < kMinInput mask has
//already claimed everything that would land here as true underflow.
constexpr float kMinReduction = -127.49f;
constexpr float kTanPolyEdge = 0.5f;

//1/k! for the Taylor of exp(f), |f| <= ln2/2.
constexpr float kInv2 = 0.5f;
constexpr float kInv6 = 1.0f / 6.0f;
constexpr float kInv24 = 1.0f / 24.0f;
constexpr float kInv120 = 1.0f / 120.0f;
constexpr float kInv720 = 1.0f / 720.0f;
constexpr float kInv5040 = 1.0f / 5040.0f;

//Odd Taylor of tanh on |x| < 0.625 (Q runs on x^2):
//tanh(x) = x*(1 - x^2/3 + 2x^4/15 - 17x^6/315 + 62x^8/2835
//             - 1382x^10/155925 + 21844x^12/6081075); the next term
//stays under 5e-8 relative at the 0.5 seam.
constexpr float kTanQ6 = 21844.0f / 6081075.0f;
constexpr float kTanQ5 = -1382.0f / 155925.0f;
constexpr float kTanQ4 = 62.0f / 2835.0f;
constexpr float kTanQ3 = -17.0f / 315.0f;
constexpr float kTanQ2 = 2.0f / 15.0f;
constexpr float kTanQ1 = -1.0f / 3.0f;

__m128 ExpF32Lanes(__m128 x) {
    const __m128 nanMask = _mm_cmpunord_ps(x, x);
    const __m128 overMask = _mm_cmpgt_ps(x, _mm_set1_ps(kMaxInput));
    const __m128 underMask = _mm_cmplt_ps(x, _mm_set1_ps(kMinInput));
    //Compute on the clamped input; the three masks above override the
    //result lanes afterwards (NaN must come back as the input itself).
    __m128 xc = _mm_min_ps(_mm_max_ps(x, _mm_set1_ps(kMinInput)),
                           _mm_set1_ps(kMaxInput));
    __m128 r = _mm_mul_ps(xc, _mm_set1_ps(kLog2e));
    r = _mm_max_ps(r, _mm_set1_ps(kMinReduction));
    const __m128i nInt = _mm_cvtps_epi32(r);
    const __m128 n = _mm_cvtepi32_ps(nInt);
    __m128 f = _mm_sub_ps(xc, _mm_mul_ps(n, _mm_set1_ps(kLn2Hi)));
    f = _mm_sub_ps(f, _mm_mul_ps(n, _mm_set1_ps(kLn2Lo)));
    __m128 p = _mm_set1_ps(kInv5040);
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(kInv720));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(kInv120));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(kInv24));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(kInv6));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(kInv2));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(1.0f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(1.0f));
    //2^n from exponent bits, with n clamped to 127 for the assembly so
    //the bit pattern never becomes inf itself; the representable-edge
    //case n==128 (result still finite) is restored by the 2x scale.
    const __m128i over128 = _mm_cmpgt_epi32(nInt, _mm_set1_epi32(127));
    const __m128i nBits = _mm_or_si128(
        _mm_andnot_si128(over128, nInt),
        _mm_and_si128(over128, _mm_set1_epi32(127)));
    const __m128i bits = _mm_slli_epi32(_mm_add_epi32(nBits,
        _mm_set1_epi32(127)), 23);
    __m128 res = _mm_mul_ps(p, _mm_castsi128_ps(bits));
    const __m128 scale = _mm_or_ps(
        _mm_and_ps(_mm_castsi128_ps(over128), _mm_set1_ps(2.0f)),
        _mm_andnot_ps(_mm_castsi128_ps(over128), _mm_set1_ps(1.0f)));
    res = _mm_mul_ps(res, scale);
    res = _mm_andnot_ps(underMask, res);
    res = _mm_or_ps(_mm_andnot_ps(overMask, res),
        _mm_and_ps(overMask,
            _mm_set1_ps(std::numeric_limits<float>::infinity())));
    return _mm_or_ps(_mm_andnot_ps(nanMask, res), _mm_and_ps(nanMask, x));
}

__m128 TanhF32Lanes(__m128 x) {
    //Large-|x| branch: the exp form saturates at +-1 by itself (the
    //exp lane's overflow to +inf feeds 2/inf = 0) and NaN propagates
    //through it; small-|x| branch: the odd Taylor.
    const __m128 twoX = _mm_add_ps(x, x);
    const __m128 e = ExpF32Lanes(twoX);
    const __m128 tExp = _mm_sub_ps(_mm_set1_ps(1.0f),
        _mm_div_ps(_mm_set1_ps(2.0f), _mm_add_ps(e, _mm_set1_ps(1.0f))));
    const __m128 x2 = _mm_mul_ps(x, x);
    __m128 q = _mm_set1_ps(kTanQ6);
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(kTanQ5));
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(kTanQ4));
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(kTanQ3));
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(kTanQ2));
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(kTanQ1));
    q = _mm_add_ps(_mm_mul_ps(q, x2), _mm_set1_ps(1.0f));
    const __m128 tPoly = _mm_mul_ps(x, q);
    const __m128 small = _mm_cmplt_ps(
        _mm_and_ps(x, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF))),
        _mm_set1_ps(kTanPolyEdge)); //|x| via the sign-bit mask
    return _mm_or_ps(_mm_andnot_ps(small, tExp), _mm_and_ps(small, tPoly));
}

} // namespace

void ExpF32Sse2(const float* src, float* dst, int64_t count) {
    int64_t i = 0;
    for (; i + 4 <= count; i += 4)
        _mm_storeu_ps(dst + i, ExpF32Lanes(_mm_loadu_ps(src + i)));
    for (; i < count; ++i) {
        dst[i] = _mm_cvtss_f32(ExpF32Lanes(_mm_set1_ps(src[i])));
    }
}

void TanhF32Sse2(const float* src, float* dst, int64_t count) {
    int64_t i = 0;
    for (; i + 4 <= count; i += 4)
        _mm_storeu_ps(dst + i, TanhF32Lanes(_mm_loadu_ps(src + i)));
    for (; i < count; ++i) {
        dst[i] = _mm_cvtss_f32(TanhF32Lanes(_mm_set1_ps(src[i])));
    }
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
