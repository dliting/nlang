/*---
    TestVecMath.cpp — vector math kernel accuracy and IEEE edge tests.

    Every test runs three times in CI (scalar/sse2/avx2 ctest entries
    set NLANG_TENSOR_TIER), so the tier dispatch inside the public
    VecMath entry points is exercised on every lane implementation.
    Accuracy reference: std::exp/std::tanh computed in double, truncated
    to the element carrier — the contract is <=4 ulp relative error for
    exp and 1e-6 relative for tanh (see the spec, ruling D6).
---*/
#include "TestHarness.h"
#include "VecMath.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace test_native_tensor {

int g_pass = 0;
int g_fail = 0;

namespace {

//Relative error against the double-computed reference, with a floor so
//near-underflow results compare by absolute gap.
double RelGap(float got, double ref) {
    const double g = static_cast<double>(got);
    const double scale = std::fabs(ref) > 1e-30 ? std::fabs(ref) : 1e-30;
    return std::fabs(g - ref) / scale;
}

void CheckExpSpecials() {
    NTT_TEST(VecExpF32_specials);
    const float qNaN = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float xs[] = {qNaN, inf, -inf, 88.721f, 88.73f, -103.972f,
                        -103.973f, 0.0f, -0.0f};
    const int n = static_cast<int>(sizeof(xs) / sizeof(xs[0]));
    std::vector<float> out(n, -1.0f);
    tensor::VecExpF32(xs, out.data(), n);
    NTT_CHECK(std::isnan(out[0]), "NaN in must give NaN out");
    NTT_CHECK(std::isinf(out[1]) && out[1] > 0.0f, "+inf must give +inf");
    NTT_CHECK(out[2] == 0.0f, "-inf must give +0");
    NTT_CHECK(!std::isinf(out[3]) && out[3] > 3.0e38f,
              "just below the overflow edge stays finite-huge like libm");
    NTT_CHECK(std::isinf(out[4]), "exp past the overflow edge is +inf");
    NTT_CHECK(out[5] >= 0.0f && out[5] < 1e-37f,
              "exp at the underflow edge is zero-or-tiny");
    NTT_CHECK(out[6] == 0.0f, "exp past the underflow edge is +0");
    NTT_CHECK(out[7] == 1.0f, "exp(0) == 1");
    NTT_CHECK(out[8] == 1.0f, "exp(-0) == 1");
    NTT_PASS();
}

void CheckExpDomain() {
    NTT_TEST(VecExpF32_matches_libm);
    std::mt19937 rng(20261111u);
    std::uniform_real_distribution<float> span(-104.0f, 89.0f);
    std::vector<float> xs(4096);
    for (float& x : xs) {
        x = span(rng);
    }
    //Boundary-adjacent points where the 2^i assembly carries.
    xs.push_back(-87.3f); xs.push_back(-1.0f); xs.push_back(1e-6f);
    xs.push_back(0.5f); xs.push_back(16.0f); xs.push_back(88.0f);
    std::vector<float> out(xs.size(), -1.0f);
    tensor::VecExpF32(xs.data(), out.data(),
                      static_cast<int64_t>(xs.size()));
    double worst = 0.0;
    const float fltMax = std::numeric_limits<float>::max();
    for (size_t i = 0; i < xs.size(); ++i) {
        const double ref = std::exp(static_cast<double>(xs[i]));
        if (ref > static_cast<double>(fltMax)) {
            //The true result leaves the float grid: inf is the answer.
            NTT_CHECK(std::isinf(out[i]) && out[i] > 0.0f,
                      "exp overflow band must give +inf at x="
                          + std::to_string(xs[i]));
            continue;
        }
        const double gap = RelGap(out[i], ref);
        if (gap > worst) {
            worst = gap;
        }
        NTT_CHECK(gap <= 4.0 * 1.1920929e-7,
                  "exp accuracy contract (worst " + std::to_string(worst)
                      + ") at x=" + std::to_string(xs[i]));
    }
    std::cerr << "    (exp worst rel gap " << worst << ")\n";
    NTT_PASS();
}

void CheckExpCountZero() {
    NTT_TEST(VecExpF32_count_zero);
    float src = 1.0f;
    float dst = -1.0f;
    tensor::VecExpF32(&src, &dst, 0);
    NTT_CHECK(dst == -1.0f, "count 0 must not touch the destination");
    NTT_PASS();
}

void CheckTanh() {
    NTT_TEST(VecTanhF32_matches_libm);
    std::mt19937 rng(42u);
    std::uniform_real_distribution<float> span(-16.0f, 16.0f);
    std::vector<float> xs(2048);
    for (float& x : xs) {
        x = span(rng);
    }
    const float qNaN = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    xs.push_back(0.0f); xs.push_back(-0.0f); xs.push_back(qNaN);
    xs.push_back(inf); xs.push_back(-inf);
    std::vector<float> out(xs.size(), -1.0f);
    tensor::VecTanhF32(xs.data(), out.data(),
                       static_cast<int64_t>(xs.size()));
    for (size_t i = 0; i < xs.size(); ++i) {
        const double ref = std::tanh(static_cast<double>(xs[i]));
        if (std::isnan(xs[i])) {
            NTT_CHECK(std::isnan(out[i]), "tanh NaN propagation");
            continue;
        }
        if (std::isinf(xs[i])) {
            NTT_CHECK(out[i] == (xs[i] > 0 ? 1.0f : -1.0f),
                      "tanh saturates at +-1 for infinities");
            continue;
        }
        NTT_CHECK(RelGap(out[i], ref) <= 1e-6,
                  "tanh accuracy contract at x=" + std::to_string(xs[i])
                      + " got " + std::to_string(out[i]));
    }
    NTT_PASS();
}

void CheckExpF64() {
    NTT_TEST(VecExpF64_matches_libm);
    const double xs[] = {-700.0, -1.0, 0.0, 1.0, 3.5, 709.0};
    const int n = static_cast<int>(sizeof(xs) / sizeof(xs[0]));
    std::vector<double> out(n, -1.0);
    tensor::VecExpF64(xs, out.data(), n);
    for (int i = 0; i < n; ++i) {
        const double ref = std::exp(xs[i]);
        const double scale = std::fabs(ref) > 1e-300 ? ref : 1e-300;
        NTT_CHECK(std::fabs(out[i] - ref) / scale <= 1e-15,
                  "double exp accuracy at x=" + std::to_string(xs[i]));
    }
    NTT_PASS();
}

} // namespace

int RunVecMathTests() {
    CheckExpSpecials();
    CheckExpDomain();
    CheckExpCountZero();
    CheckTanh();
    CheckExpF64();
    std::cerr << "vecmath suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
