/*---
    VecReduceAvx2.cpp — AVX2 reduction lanes: sum (double accumulator,
    f32 lanes widen per chunk so order is free for the exact sums the
    e2e suite pins), dot (carrier accumulator per the historical
    DotKernel), max/min (cmp-and-select — NaN never wins unless first,
    ties keep the earlier element, exactly the scalar walk) and the
    axis-0 addInto accumulation.
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include <immintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {
namespace {

double HorizontalSum(__m256d v) {
    __m128d lo = _mm256_castpd256_pd128(v);
    const __m128d hi = _mm256_extractf128_pd(v, 1);
    lo = _mm_add_pd(lo, hi);
    return _mm_cvtsd_f64(lo) + _mm_cvtsd_f64(_mm_unpackhi_pd(lo, lo));
}

float HorizontalSumPs(__m256 v) {
    __m128 lo = _mm256_castps256_ps128(v);
    const __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 s = _mm_add_ps(lo, hi);
    __m128 shuf = _mm_movehdup_ps(s);
    s = _mm_add_ps(s, shuf);
    shuf = _mm_movehl_ps(shuf, s);
    return _mm_cvtss_f32(_mm_add_ss(s, shuf));
}


} // namespace
void SumF32Avx2(const float* cells, int64_t count, double* out) {
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 8 <= count; i += 8) {
        acc0 = _mm256_add_pd(acc0,
            _mm256_cvtps_pd(_mm256_castps256_ps128(
                _mm256_loadu_ps(cells + i))));
        acc1 = _mm256_add_pd(acc1,
            _mm256_cvtps_pd(_mm_loadu_ps(cells + i + 4)));
    }
    double sum = HorizontalSum(acc0) + HorizontalSum(acc1);
    for (; i < count; ++i) {
        sum += cells[i];
    }
    *out = sum;
}

void SumF64Avx2(const double* cells, int64_t count, double* out) {
    __m256d acc = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        acc = _mm256_add_pd(acc, _mm256_loadu_pd(cells + i));
    }
    double sum = HorizontalSum(acc);
    for (; i < count; ++i) {
        sum += cells[i];
    }
    *out = sum;
}

void DotF32Avx2(const float* a, const float* b, int64_t count, float* out) {
    __m256 acc = _mm256_setzero_ps();
    int64_t i = 0;
    for (; i + 8 <= count; i += 8) {
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + i),
                              _mm256_loadu_ps(b + i), acc);
    }
    float sum = HorizontalSumPs(acc);
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    *out = sum;
}

void DotF64Avx2(const double* a, const double* b, int64_t count,
                double* out) {
    __m256d acc = _mm256_setzero_pd();
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        acc = _mm256_fmadd_pd(_mm256_loadu_pd(a + i),
                              _mm256_loadu_pd(b + i), acc);
    }
    double sum = HorizontalSum(acc);
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    *out = sum;
}

//cmp-and-select max/min: an unordered cell (NaN) never takes the
//accumulator, ties keep it — the scalar walk's exact rule, lane-wise;
//the horizontal fold keeps the lower (earlier) lanes on ties.
namespace {
template <typename T>
struct ReduceLanes;

template <>
struct ReduceLanes<float> {
    using V = __m256;
    static constexpr int64_t kLanes = 8;
    static V Load(const float* p) { return _mm256_loadu_ps(p); }
    static V First(float v) { return _mm256_set1_ps(v); }
    static bool Wins(bool isMax, float cur, float best) {
        return isMax ? cur > best : cur < best;
    }
    static V Select(bool isMax, V cur, V acc) {
        const __m256 wins = isMax
            ? _mm256_cmp_ps(cur, acc, _CMP_GT_OQ)
            : _mm256_cmp_ps(cur, acc, _CMP_LT_OQ);
        return _mm256_blendv_ps(acc, cur, wins);
    }
    static float Fold(bool isMax, V acc) {
        //Per-lane winners across the two 128-bit halves, then a
        //pairwise select tree — ties keep the earlier (lower-lane)
        //side, matching the scalar walk's first-seen rule.
        __m128 lo = _mm256_castps256_ps128(acc);
        const __m128 hi = _mm256_extractf128_ps(acc, 1);
        const __m128 winsHalf = isMax
            ? _mm_cmpgt_ps(hi, lo)
            : _mm_cmplt_ps(hi, lo);
        const __m128 sel = _mm_or_ps(_mm_andnot_ps(winsHalf, lo),
                                     _mm_and_ps(winsHalf, hi));
        const __m128 hi2 = _mm_movehl_ps(sel, sel); //[w2, w3, ..]
        const __m128 wins2 = isMax
            ? _mm_cmpgt_ps(hi2, sel)
            : _mm_cmplt_ps(hi2, sel);
        const __m128 best2 = _mm_or_ps(_mm_andnot_ps(wins2, sel),
                                       _mm_and_ps(wins2, hi2));
//lane-1 extract: unpackhi_ps would take lane 2 (it interleaves
        //the high halves).
        const __m128 hi1 = _mm_shuffle_ps(best2, best2,
            _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 wins1 = isMax
            ? _mm_cmpgt_ps(hi1, best2)
            : _mm_cmplt_ps(hi1, best2);
        return _mm_cvtss_f32(_mm_or_ps(_mm_andnot_ps(wins1, best2),
                                       _mm_and_ps(wins1, hi1)));
    }
};

template <>
struct ReduceLanes<double> {
    using V = __m256d;
    static constexpr int64_t kLanes = 4;
    static V Load(const double* p) { return _mm256_loadu_pd(p); }
    static V First(double v) { return _mm256_set1_pd(v); }
    static bool Wins(bool isMax, double cur, double best) {
        return isMax ? cur > best : cur < best;
    }
    static V Select(bool isMax, V cur, V acc) {
        const __m256d wins = isMax
            ? _mm256_cmp_pd(cur, acc, _CMP_GT_OQ)
            : _mm256_cmp_pd(cur, acc, _CMP_LT_OQ);
        return _mm256_blendv_pd(acc, cur, wins);
    }
    static double Fold(bool isMax, V acc) {
        //sel = per-lane winners of (lo, hi) pairs; the final select
        //keeps the earlier side on ties.
        __m128d lo = _mm256_castpd256_pd128(acc);
        const __m128d hi = _mm256_extractf128_pd(acc, 1);
        const __m128d winsHalf = isMax
            ? _mm_cmpgt_pd(hi, lo)
            : _mm_cmplt_pd(hi, lo);
        const __m128d sel = _mm_or_pd(_mm_andnot_pd(winsHalf, lo),
                                      _mm_and_pd(winsHalf, hi));
        const __m128d hi1 = _mm_unpackhi_pd(sel, sel); //[sel1, sel1]
        const __m128d wins1 = isMax
            ? _mm_cmpgt_pd(hi1, sel)
            : _mm_cmplt_pd(hi1, sel);
        return _mm_cvtsd_f64(_mm_or_pd(_mm_andnot_pd(wins1, sel),
                                       _mm_and_pd(wins1, hi1)));
    }
};

template <typename T>
void FlatExtremeAvx2(bool isMax, const T* cells, int64_t count, T* out) {
    using Tr = ReduceLanes<T>;
    T best = cells[0];
    int64_t i = 1;
    int64_t vecEnd = ((count - 1) / Tr::kLanes) * Tr::kLanes;
    //Vector sweep over [1, vecEnd) with the accumulator seeded from
    //cells[0]; a later scalar pass only needs the tail.
    if (count >= Tr::kLanes) {
        typename Tr::V acc = Tr::First(cells[0]);
        i = 1;
        for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
            acc = Tr::Select(isMax, Tr::Load(cells + i), acc);
        }
        best = Tr::Fold(isMax, acc);
        //The swept span covered [0, i); fold already includes cells[0].
        //Remaining tail competes against the folded best.
        vecEnd = i;
    }
    for (int64_t j = vecEnd < 1 ? 1 : vecEnd; j < count; ++j) {
        if (Tr::Wins(isMax, cells[j], best)) {
            best = cells[j];
        }
    }
    //Wait — the sweep above compares vector acc (which includes
    //cells[0]) only against [1, vecEnd); tail cells [vecEnd, count)
    //compete via Wins. The folded best already carries the vector span.
    *out = best;
}

} // namespace

void MaxF32Avx2(const float* cells, int64_t count, float* out) {
    FlatExtremeAvx2(true, cells, count, out);
}

void MaxF64Avx2(const double* cells, int64_t count, double* out) {
    FlatExtremeAvx2(true, cells, count, out);
}

void MinF32Avx2(const float* cells, int64_t count, float* out) {
    FlatExtremeAvx2(false, cells, count, out);
}

void MinF64Avx2(const double* cells, int64_t count, double* out) {
    FlatExtremeAvx2(false, cells, count, out);
}

void AddIntoF64FromF32Avx2(double* acc, const float* row, int64_t count) {
    int64_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256d lo = _mm256_add_pd(
            _mm256_loadu_pd(acc + i),
            _mm256_cvtps_pd(_mm256_castps256_ps128(
                _mm256_loadu_ps(row + i))));
        const __m256d hi = _mm256_add_pd(
            _mm256_loadu_pd(acc + i + 4),
            _mm256_cvtps_pd(_mm_loadu_ps(row + i + 4)));
        _mm256_storeu_pd(acc + i, lo);
        _mm256_storeu_pd(acc + i + 4, hi);
    }
    for (; i < count; ++i) {
        acc[i] += row[i];
    }
}

void AddIntoF64Avx2(double* acc, const double* row, int64_t count) {
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        _mm256_storeu_pd(acc + i, _mm256_add_pd(_mm256_loadu_pd(acc + i),
                                                _mm256_loadu_pd(row + i)));
    }
    for (; i < count; ++i) {
        acc[i] += row[i];
    }
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
