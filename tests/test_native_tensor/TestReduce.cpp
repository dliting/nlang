/*---
    TestReduce.cpp — reduction kernel tier cross-checks: sum (double
    accumulator), dot (carrier accumulator), max/min with the scalar
    NaN-and-tie semantics, and the axis-0 addInto accumulation.
    Max/min must reproduce the scalar walk exactly: a NaN cell never
    wins unless it was first, and ties keep the earlier element (the
    cmp-and-select kernels, not maxps).
---*/
#include "TestHarness.h"
#include "TensorBackends.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace test_native_tensor {

namespace {

template <typename T>
void CheckSum(int64_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> span(-1.0, 1.0);
    std::vector<T> x(n);
    for (T& v : x) {
        v = static_cast<T>(span(rng));
    }
    double want = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        want += static_cast<double>(x[i]); //the language's double acc
    }
    double got = 0.0;
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().sumF32(x.data(), n, &got);
    } else {
        tensor::ActiveKernelOps().sumF64(x.data(), n, &got);
    }
    const double scale = std::max(1.0, std::fabs(want));
    const double gap = std::fabs(got - want) / scale;
    NTT_CHECK(gap <= 1e-12,
              std::string("sum (worst ") + std::to_string(gap) + ")");
}

template <typename T>
void CheckDot(int64_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> span(-1.0, 1.0);
    std::vector<T> x(n), y(n);
    for (T& v : x) {
        v = static_cast<T>(span(rng));
    }
    for (T& v : y) {
        v = static_cast<T>(span(rng));
    }
    T want = T(0);
    for (int64_t i = 0; i < n; ++i) {
        want = static_cast<T>(want + x[i] * y[i]); //carrier acc
    }
    T got = T(0);
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().dotF32(x.data(), y.data(), n, &got);
    } else {
        tensor::ActiveKernelOps().dotF64(x.data(), y.data(), n, &got);
    }
    const double budget = 1e-4
        + 2e-5 * std::fabs(static_cast<double>(want));
    NTT_CHECK(std::fabs(static_cast<double>(got)
                  - static_cast<double>(want)) <= budget,
              std::string("dot (got ") + std::to_string(got) + ", want "
                  + std::to_string(want) + ")");
}

//Exact-order max: NaN never wins (unless first), ties keep the earlier
//element — the contract the scalar walk has always had.
template <typename T>
void CheckMaxSemantics() {
    NTT_TEST(MaxMin_nan_and_tie_semantics);
    const T qNaN = std::numeric_limits<T>::quiet_NaN();
    //layout: normal, NaN, larger, -0.0 after 0.0 (tie keeps earlier)
    const T x[] = {T(1), qNaN, T(3), T(0), T(-0.0)};
    T got = T(0);
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().maxF32(x, 5, &got);
    } else {
        tensor::ActiveKernelOps().maxF64(x, 5, &got);
    }
    NTT_CHECK(got == T(3), "max skips NaN, finds 3");
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().minF32(x, 5, &got);
    } else {
        tensor::ActiveKernelOps().minF64(x, 5, &got);
    }
    NTT_CHECK(got == T(-0.0), "min tie keeps the earlier -0.0");
    //NaN first must poison the whole reduction (scalar rule).
    const T y[] = {qNaN, T(1), T(2)};
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().maxF32(y, 3, &got);
    } else {
        tensor::ActiveKernelOps().maxF64(y, 3, &got);
    }
    NTT_CHECK(std::isnan(static_cast<double>(got)),
              "NaN first must poison max like the scalar walk");
    NTT_PASS();
}

void CheckAddIntoAxisSum() {
    NTT_TEST(AddInto_axis0_matches_reference);
    //rank-2 5x7 contiguous: out[j] = sum_i x[i][j] in double.
    std::mt19937 rng(23u);
    std::uniform_real_distribution<float> span(-1.0f, 1.0f);
    const int32_t rows = 5;
    const int32_t cols = 7;
    std::vector<float> x(rows * cols);
    for (float& v : x) {
        v = span(rng);
    }
    std::vector<double> want(cols, 0.0);
    for (int32_t j = 0; j < cols; ++j) {
        for (int32_t i = 0; i < rows; ++i) {
            want[j] += x[static_cast<int64_t>(i) * cols + j];
        }
    }
    std::vector<double> got(cols, 0.0);
    for (int32_t i = 0; i < rows; ++i) {
        tensor::ActiveKernelOps().addIntoF64FromF32(
            got.data(), x.data() + static_cast<int64_t>(i) * cols, cols);
    }
    double worst = 0.0;
    for (int32_t j = 0; j < cols; ++j) {
        worst = std::max(worst, std::fabs(got[j] - want[j]));
    }
    NTT_CHECK(worst <= 1e-12, "axis-0 accumulation (worst "
        + std::to_string(worst) + ")");
    NTT_PASS();
}

} // namespace

int RunReduceTests() {
    CheckSum<float>(4096, 31u);
    CheckSum<double>(4096, 32u);
    CheckDot<float>(4096, 33u);
    CheckDot<double>(4096, 34u);
    CheckMaxSemantics<float>();
    CheckMaxSemantics<double>();
    CheckAddIntoAxisSum();
    std::cerr << "reduce suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
