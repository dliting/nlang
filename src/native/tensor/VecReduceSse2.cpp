/*---
    VecReduceSse2.cpp — SSE2 reduction lanes (x64 baseline): same
    contracts as VecReduceAvx2.cpp, 4/2-wide, mul+add instead of FMA,
    and/andnot instead of blends.
---*/
#include "VecMathTiers.h"

#if defined(NLANG_SIMD_X64)
#include <emmintrin.h>

#include <cstdint>

namespace tensor {
namespace detail {
namespace {

double HorizontalSum(__m128d v) {
    return _mm_cvtsd_f64(v) + _mm_cvtsd_f64(_mm_unpackhi_pd(v, v));
}

float HorizontalSumPs(__m128 v) {
    v = _mm_add_ps(v, _mm_movehl_ps(v, v));
    return _mm_cvtss_f32(_mm_add_ss(v, _mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 1, 1, 1))));
}
} // namespace

void SumF32Sse2(const float* cells, int64_t count, double* out) {
    __m128d acc0 = _mm_setzero_pd();
    __m128d acc1 = _mm_setzero_pd();
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        acc0 = _mm_add_pd(acc0,
            _mm_cvtps_pd(_mm_loadu_ps(cells + i)));
        acc1 = _mm_add_pd(acc1,
            _mm_cvtps_pd(_mm_loadu_ps(cells + i + 2)));
    }
    double sum = HorizontalSum(acc0) + HorizontalSum(acc1);
    for (; i < count; ++i) {
        sum += cells[i];
    }
    *out = sum;
}

void SumF64Sse2(const double* cells, int64_t count, double* out) {
    __m128d acc = _mm_setzero_pd();
    int64_t i = 0;
    for (; i + 2 <= count; i += 2) {
        acc = _mm_add_pd(acc, _mm_loadu_pd(cells + i));
    }
    double sum = HorizontalSum(acc);
    for (; i < count; ++i) {
        sum += cells[i];
    }
    *out = sum;
}

void DotF32Sse2(const float* a, const float* b, int64_t count, float* out) {
    __m128 acc = _mm_setzero_ps();
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        acc = _mm_add_ps(acc,
            _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
    }
    float sum = HorizontalSumPs(acc);
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    *out = sum;
}

void DotF64Sse2(const double* a, const double* b, int64_t count,
                double* out) {
    __m128d acc = _mm_setzero_pd();
    int64_t i = 0;
    for (; i + 2 <= count; i += 2) {
        acc = _mm_add_pd(acc,
            _mm_mul_pd(_mm_loadu_pd(a + i), _mm_loadu_pd(b + i)));
    }
    double sum = HorizontalSum(acc);
    for (; i < count; ++i) {
        sum += a[i] * b[i];
    }
    *out = sum;
}

//cmp-and-select extreme with the scalar walk's NaN/tie rule.
template <typename T>
struct SseReduceLanes;

template <>
struct SseReduceLanes<float> {
    using V = __m128;
    static constexpr int64_t kLanes = 4;
    static V Load(const float* p) { return _mm_loadu_ps(p); }
    static V First(float v) { return _mm_set1_ps(v); }
    static bool Wins(bool isMax, float cur, float best) {
        return isMax ? cur > best : cur < best;
    }
    static V Select(bool isMax, V cur, V acc) {
        const __m128 wins = isMax
            ? _mm_cmpgt_ps(cur, acc)
            : _mm_cmplt_ps(cur, acc);
        return _mm_or_ps(_mm_andnot_ps(wins, acc), _mm_and_ps(wins, cur));
    }
    static float Fold(bool isMax, V acc) {
        const __m128 hi = _mm_movehl_ps(acc, acc); //[w2, w3, ..]
        const __m128 wins2 = isMax
            ? _mm_cmpgt_ps(hi, acc)
            : _mm_cmplt_ps(hi, acc);
        const __m128 sel = _mm_or_ps(_mm_andnot_ps(wins2, acc),
                                     _mm_and_ps(wins2, hi));
//lane-1 extract: unpackhi_ps would take lane 2 (it interleaves
        //the high halves) — the pd twin is 2-lane and gets away with it.
        const __m128 hi1 = _mm_shuffle_ps(sel, sel,
            _MM_SHUFFLE(1, 1, 1, 1));
        const __m128 wins1 = isMax
            ? _mm_cmpgt_ps(hi1, sel)
            : _mm_cmplt_ps(hi1, sel);
        return _mm_cvtss_f32(_mm_or_ps(_mm_andnot_ps(wins1, sel),
                                       _mm_and_ps(wins1, hi1)));
    }
};

template <>
struct SseReduceLanes<double> {
    using V = __m128d;
    static constexpr int64_t kLanes = 2;
    static V Load(const double* p) { return _mm_loadu_pd(p); }
    static V First(double v) { return _mm_set1_pd(v); }
    static bool Wins(bool isMax, double cur, double best) {
        return isMax ? cur > best : cur < best;
    }
    static V Select(bool isMax, V cur, V acc) {
        const __m128d wins = isMax
            ? _mm_cmpgt_pd(cur, acc)
            : _mm_cmplt_pd(cur, acc);
        return _mm_or_pd(_mm_andnot_pd(wins, acc), _mm_and_pd(wins, cur));
    }
    static double Fold(bool isMax, V acc) {
        const __m128d hi = _mm_unpackhi_pd(acc, acc);
        const __m128d wins = isMax
            ? _mm_cmpgt_pd(hi, acc)
            : _mm_cmplt_pd(hi, acc);
        return _mm_cvtsd_f64(_mm_or_pd(_mm_andnot_pd(wins, acc),
                                       _mm_and_pd(wins, hi)));
    }
};

namespace {

template <typename T>
void FlatExtremeSse2(bool isMax, const T* cells, int64_t count, T* out) {
    using Tr = SseReduceLanes<T>;
    T best = cells[0];
    int64_t swept = 1;
    if (count >= Tr::kLanes) {
        typename Tr::V acc = Tr::First(cells[0]);
        int64_t i = 1;
        for (; i + Tr::kLanes <= count; i += Tr::kLanes) {
            acc = Tr::Select(isMax, Tr::Load(cells + i), acc);
        }
        best = Tr::Fold(isMax, acc);
        swept = i;
    }
    for (int64_t j = swept; j < count; ++j) {
        if (Tr::Wins(isMax, cells[j], best)) {
            best = cells[j];
        }
    }
    *out = best;
}

} // namespace

void MaxF32Sse2(const float* cells, int64_t count, float* out) {
    FlatExtremeSse2(true, cells, count, out);
}

void MaxF64Sse2(const double* cells, int64_t count, double* out) {
    FlatExtremeSse2(true, cells, count, out);
}

void MinF32Sse2(const float* cells, int64_t count, float* out) {
    FlatExtremeSse2(false, cells, count, out);
}

void MinF64Sse2(const double* cells, int64_t count, double* out) {
    FlatExtremeSse2(false, cells, count, out);
}

void AddIntoF64FromF32Sse2(double* acc, const float* row, int64_t count) {
    int64_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m128d lo = _mm_add_pd(_mm_loadu_pd(acc + i),
            _mm_cvtps_pd(_mm_loadu_ps(row + i)));
        const __m128d hi = _mm_add_pd(_mm_loadu_pd(acc + i + 2),
            _mm_cvtps_pd(_mm_loadu_ps(row + i + 2)));
        _mm_storeu_pd(acc + i, lo);
        _mm_storeu_pd(acc + i + 2, hi);
    }
    for (; i < count; ++i) {
        acc[i] += row[i];
    }
}

void AddIntoF64Sse2(double* acc, const double* row, int64_t count) {
    int64_t i = 0;
    for (; i + 2 <= count; i += 2) {
        _mm_storeu_pd(acc + i,
            _mm_add_pd(_mm_loadu_pd(acc + i), _mm_loadu_pd(row + i)));
    }
    for (; i < count; ++i) {
        acc[i] += row[i];
    }
}

} // namespace detail
} // namespace tensor

#endif //NLANG_SIMD_X64
