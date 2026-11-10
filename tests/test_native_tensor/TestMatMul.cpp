/*---
    TestMatMul.cpp — matmul kernel tier cross-checks.

    The reference is the plain i-k-j scalar loop compiled here; every
    tier run (scalar/sse2/avx2 ctest entries set NLANG_TENSOR_TIER)
    must match it within tolerance (FMA drops one rounding, so float
    compares with a relative tolerance, ints compare exactly).
    Shape matrix covers the degenerate edges (1x1, inner=1) and the
    MNIST workload shapes plus a non-multiple-of-tile sweep.
---*/
#include "TestHarness.h"
#include "TensorBackends.h"

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace test_native_tensor {

namespace {

//Typed dispatch onto the backend table's matmul slots.
template <typename T>
void CallMatMul(int32_t rows, int32_t inner, int32_t cols,
                const T* a, const T* b, T* out) {
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().matmulF32(rows, inner, cols, a, b, out);
    } else {
        tensor::ActiveKernelOps().matmulF64(rows, inner, cols, a, b, out);
    }
}

//Plain i-k-j reference (the language-wide arithmetic rule: ints wrap,
//floats accumulate in the carrier).
template <typename T>
void NaiveMatMul(int32_t rows, int32_t inner, int32_t cols,
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

template <typename T>
void CheckAgainstReference(const char* what, int32_t rows, int32_t inner,
                           int32_t cols, double atol, double relTol,
                           unsigned seed) {
    const int64_t an = static_cast<int64_t>(rows) * inner;
    const int64_t bn = static_cast<int64_t>(inner) * cols;
    const int64_t cn = static_cast<int64_t>(rows) * cols;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> span(-1.0, 1.0);
    std::vector<T> a(an), b(bn), got(cn, T(0)), want(cn, T(0));
    for (int64_t i = 0; i < an; ++i)
        a[i] = static_cast<T>(span(rng));
    for (int64_t i = 0; i < bn; ++i)
        b[i] = static_cast<T>(span(rng));
    NaiveMatMul<T>(rows, inner, cols, a.data(), b.data(), want.data());
    //The contract is accumulate-into-zeroed; got starts zeroed.
    CallMatMul<T>(rows, inner, cols, a.data(), b.data(), got.data());
    //Mixed metric (numpy-allclose shaped): near-zero results of
    //cancellation-heavy dot products compare by an absolute budget,
    //not a pure relative gap.
    int64_t badAt = -1;
    double worst = 0.0;
    for (int64_t i = 0; i < cn; ++i) {
        const double g = static_cast<double>(got[i]);
        const double w = static_cast<double>(want[i]);
        const double budget = atol + relTol * std::fabs(w);
        const double used = std::fabs(g - w) / budget;
        if (used > worst) {
            worst = used;
            badAt = i;
        }
    }
    NTT_CHECK(worst <= 1.0,
              std::string(what) + " vs reference (worst budget use "
                  + std::to_string(worst) + " at #" + std::to_string(badAt)
                  + ")");
}

void CheckEmptyNoOp() {
    NTT_TEST(MatMulFlat_empty_noop);
    float scratch = -1.0f;
    tensor::ActiveKernelOps().matmulF32(0, 5, 7, nullptr, nullptr, &scratch);
    NTT_CHECK(scratch == -1.0f, "rows=0 must not write");
    //inner=0 with real geometry: the accumulation of nothing — a
    //zeroed output must stay zero (scalar lanes skip the store, vector
    //lanes store their zero accumulators; both agree on the zeroed
    //storage the driver hands over).
    float out[3] = {0.0f, 0.0f, 0.0f};
    const float aRow[2] = {1.0f, 2.0f};
    tensor::ActiveKernelOps().matmulF32(1, 0, 3, aRow, nullptr, out);
    NTT_CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f,
              "inner=0 must accumulate nothing into zeroed storage");
    NTT_PASS();
}

//Ints wrap like language int arithmetic — exact equality (a void
//helper: NTT_CHECK's early return is void).
void CheckIntWrapExact() {
    NTT_TEST(MatMulFlat_i32_wraps_exactly);
    const int32_t a[] = {2000000000, 3, -5, 7};
    const int32_t b[] = {2, -1, 1000000000, 4};
    int32_t got[4] = {0, 0, 0, 0};
    int32_t want[4] = {0, 0, 0, 0};
    tensor::ActiveKernelOps().matmulI32(2, 2, 2, a, b, got);
    NaiveMatMul<int32_t>(2, 2, 2, a, b, want);
    NTT_CHECK(got[0] == want[0] && got[1] == want[1]
                  && got[2] == want[2] && got[3] == want[3],
              "int matmul must wrap identically to the scalar rule");
    NTT_PASS();
}

} // namespace

int RunMatMulTests() {
    struct Shape { int32_t rows, inner, cols; };
    const Shape shapes[] = {
        {1, 1, 1}, {1, 7, 1}, {3, 1, 5}, {7, 9, 8},
        {32, 784, 100}, {33, 65, 17}, {64, 64, 64}, {63, 127, 33},
        {512, 256, 512},
    };
    for (const Shape& s : shapes) {
        NTT_TEST(MatMulFlat_f32_shapes);
        //f32: atol covers K-term f32 accumulation noise (K*eps*|term|
        //~1e-4 at K=784); rtol covers large outputs. The FMA kernels
        //are more accurate than the naive reference — the budget
        //absorbs the reference's own rounding.
        CheckAgainstReference<float>("matmul f32", s.rows, s.inner,
                                     s.cols, 1e-4, 2e-5, 7u);
        NTT_PASS();
        NTT_TEST(MatMulFlat_f64_shapes);
        CheckAgainstReference<double>("matmul f64", s.rows, s.inner,
                                      s.cols, 1e-9, 2e-5, 11u);
        NTT_PASS();
    }
    CheckIntWrapExact();
    CheckEmptyNoOp();
    std::cerr << "matmul suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
