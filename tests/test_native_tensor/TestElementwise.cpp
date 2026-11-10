/*---
    TestElementwise.cpp — binary/unary/clamp kernel tier cross-checks
    plus the driver's fast-path decisions (contiguous flat vs strided
    view vs single-cell broadcast).
    References are the scalar lanes the backend itself uses for strided
    walks; floats compare under an atol+rtol budget (div/IEEE specials
    assert exactly).
---*/
#include "TestHarness.h"
#include "TensorBackends.h"

#include "Elementwise.h"
#include "VecMath.h"

#include <nlang/vm/NativeHost.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace test_native_tensor {

namespace {

//The authoritative scalar lane semantics (must mirror the backend's
//strided walk; the shared truth lives in Elementwise.h's ScalarLane,
//which the driver tests below exercise through the walk path).
double ScalarBinary(tensor::BinaryKernelOp op, double a, double b) {
    switch (op) {
        case tensor::BinaryKernelOp::Add: return a + b;
        case tensor::BinaryKernelOp::Sub: return a - b;
        case tensor::BinaryKernelOp::Mul: return a * b;
        case tensor::BinaryKernelOp::Div: return a / b;
    }
    return 0.0;
}

template <typename T>
void CheckFlatBinary(tensor::BinaryKernelOp op, int64_t n, double atol,
                     double rtol, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> span(-2.0, 2.0);
    std::vector<T> a(n), b(n), got(n, T(0));
    for (int64_t i = 0; i < n; ++i) {
        a[i] = static_cast<T>(span(rng));
        b[i] = static_cast<T>(span(rng));
    }
    if constexpr (std::is_same_v<T, float>) {
        tensor::ActiveKernelOps().binaryF32(op, a.data(), b.data(),
                                            got.data(), n);
    } else {
        tensor::ActiveKernelOps().binaryF64(op, a.data(), b.data(),
                                            got.data(), n);
    }
    double worst = 0.0;
    int64_t badAt = -1;
    for (int64_t i = 0; i < n; ++i) {
        const double w = ScalarBinary(op, a[i], b[i]);
        const double g = static_cast<double>(got[i]);
        const double used = std::fabs(g - w)
            / (atol + rtol * std::fabs(w));
        if (used > worst) {
            worst = used;
            badAt = i;
        }
    }
    NTT_CHECK(worst <= 1.0,
              std::string("flat binary vs scalar (budget use ")
                  + std::to_string(worst) + " at #" + std::to_string(badAt)
                  + ")");
}

void CheckDivIeeeEdges() {
    NTT_TEST(BinaryF32_div_ieee_edges);
    const float inf = std::numeric_limits<float>::infinity();
    const float qNaN = std::numeric_limits<float>::quiet_NaN();
    const float a[] = {1.0f, 0.0f, -1.0f, 0.0f};
    const float b[] = {0.0f, 0.0f, 0.0f, qNaN};
    float got[4] = {};
    tensor::ActiveKernelOps().binaryF32(tensor::BinaryKernelOp::Div,
                                        a, b, got, 4);
    NTT_CHECK(std::isinf(got[0]) && got[0] > 0, "1/0 -> +inf");
    NTT_CHECK(std::isnan(got[1]), "0/0 -> NaN");
    NTT_CHECK(std::isinf(got[2]) && got[2] < 0, "-1/0 -> -inf");
    NTT_CHECK(std::isnan(got[3]), "x/NaN -> NaN");
    NTT_PASS();
}

void CheckBinaryScalarArm() {
    NTT_TEST(BinaryScalarF32_matches);
    std::mt19937 rng(5u);
    std::uniform_real_distribution<float> span(-3.0f, 3.0f);
    const int64_t n = 1000;
    std::vector<float> a(n), got(n, 0.0f);
    for (float& x : a) {
        x = span(rng);
    }
    const float s = 1.75f;
    tensor::ActiveKernelOps().binaryScalarF32(
        tensor::BinaryKernelOp::Mul, a.data(), s, got.data(), n);
    double worst = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        worst = std::max(worst, std::fabs(
            static_cast<double>(got[i]) - static_cast<double>(a[i]) * s));
    }
    NTT_CHECK(worst <= 1e-5, "scalar-arm mul (worst "
        + std::to_string(worst) + ")");
    NTT_PASS();
}

void CheckClamp() {
    NTT_TEST(ClampF32_relu_shape);
    const float a[] = {-2.0f, -0.0f, 0.0f, 0.5f, 3.0f};
    float gotMax[5] = {};
    float gotMin[5] = {};
    tensor::ActiveKernelOps().clampF32(true, a, 0.0f, gotMax, 5);
    tensor::ActiveKernelOps().clampF32(false, a, 0.0f, gotMin, 5);
    NTT_CHECK(gotMax[0] == 0.0f && gotMax[1] == 0.0f
                  && gotMax[2] == 0.0f && gotMax[3] == 0.5f
                  && gotMax[4] == 3.0f,
              "clampMax(a, 0) == relu semantics");
    NTT_CHECK(gotMin[0] == -2.0f && gotMin[1] == -0.0f
                  && gotMin[2] == 0.0f && gotMin[3] == 0.0f
                  && gotMin[4] == 0.0f,
              "clampMin(a, 0) floors from above");
    NTT_PASS();
}

void CheckUnaryLog() {
    NTT_TEST(UnaryF32_log_matches_libm);
    std::mt19937 rng(9u);
    std::uniform_real_distribution<double> span(1e-6, 1e6);
    const int64_t n = 512;
    std::vector<float> x(n), got(n, 0.0f);
    for (float& v : x) {
        v = static_cast<float>(span(rng));
    }
    tensor::ActiveKernelOps().unaryF32(tensor::UnaryKernelOp::Log,
                                       x.data(), got.data(), n);
    double worst = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        const double ref = std::log(static_cast<double>(x[i]));
        const double gap = std::fabs(static_cast<double>(got[i]) - ref)
            / (1e-9 + std::fabs(ref));
        worst = std::max(worst, gap);
    }
    NTT_CHECK(worst <= 1e-6, "log (worst " + std::to_string(worst) + ")");
    NTT_PASS();
}

//--- driver-level tests: real NativeArrayView shapes ------------------------

void CheckDriverStridedUnary() {
    NTT_TEST(Driver_strided_view_uses_walk);
    //A stride-2 view over an owner buffer; the exp result must match
    //the contiguous exp of the same logical elements.
    std::mt19937 rng(13u);
    std::uniform_real_distribution<float> span(-1.0f, 1.0f);
    const int64_t count = 257;
    std::vector<float> owner(count * 2);
    for (float& v : owner) {
        v = span(rng);
    }
    const int32_t dims1[1] = {static_cast<int32_t>(count)};
    NativeArrayView view = {};
    view.data = owner.data();
    view.dims = dims1;
    view.strides = view.stridesInline;
    view.stridesInline[0] = 2;
    view.offset = 3;
    view.rank = 1;
    view.elemKind = 1; //NEK_Float32
    view.elemCount = count;
    view.elemCellBytes = 4;
    std::vector<float> logical(count);
    for (int64_t i = 0; i < count; ++i) {
        logical[i] = owner[3 + 2 * i];
    }
    std::vector<float> got(count, 0.0f);
    std::vector<float> want(count, 0.0f);
    NativeArrayView outView = {};
    outView.data = got.data();
    outView.dims = dims1;
    outView.strides = outView.stridesInline;
    outView.stridesInline[0] = 1;
    outView.offset = 0;
    outView.rank = 1;
    outView.elemKind = 1;
    outView.elemCount = count;
    outView.elemCellBytes = 4;
    tensor::RunUnaryFlat<float>(view, outView, tensor::UnaryKernelOp::Exp);
    tensor::VecExpF32(logical.data(), want.data(), count);
    //The walk rides the libm-exact scalar lane while the contiguous
    //span rides the polynomial flat lane — the contract is the VecMath
    //tolerance (<=4 ulp), not bit equality; what this test pins is the
    //walk's element addressing (stride 2 + offset 3).
    double worst = 0.0;
    for (int64_t i = 0; i < count; ++i) {
        const double scale = std::max(1.0,
            std::fabs(static_cast<double>(want[i])));
        worst = std::max(worst,
            std::fabs(static_cast<double>(got[i])
                - static_cast<double>(want[i])) / scale);
    }
    NTT_CHECK(worst <= 4.0 * 1.1920929e-7,
              "strided walk addressing within the exp contract ("
                  + std::to_string(worst) + ")");
    NTT_PASS();
}

void CheckDriverBroadcastSingleCell() {
    NTT_TEST(Driver_single_cell_operand_broadcasts);
    float aCells[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    float oneCell[1] = {10.0f};
    float got[6] = {};
    const int32_t dims6[2] = {2, 3};
    const int32_t dims1[1] = {1};
    NativeArrayView va = {};
    va.data = aCells;
    va.dims = dims6;
    va.strides = va.stridesInline;
    va.stridesInline[0] = 3;
    va.stridesInline[1] = 1;
    va.offset = 0;
    va.rank = 2;
    va.elemKind = 1;
    va.elemCount = 6;
    va.elemCellBytes = 4;
    NativeArrayView vb = va;
    vb.data = oneCell;
    vb.dims = dims1;
    vb.strides = vb.stridesInline; //a copied view must re-own its strides
    vb.stridesInline[0] = 1;
    vb.elemCount = 1;
    NativeArrayView out = va;
    out.data = got;
    out.strides = out.stridesInline;
    tensor::TryRunBinaryFlat<float>(va, vb, out,
                                 tensor::BinaryKernelOp::Mul);
    bool ok = true;
    for (int i = 0; i < 6; ++i) {
        ok = ok && got[i] == aCells[i] * 10.0f;
    }
    NTT_CHECK(ok, "single-cell b must broadcast through the scalar arm");
    NTT_PASS();
}

} // namespace

int RunElementwiseTests() {
    for (int op = 0; op <= 3; ++op) {
        NTT_TEST(BinaryF32_flat_ops);
        CheckFlatBinary<float>(static_cast<tensor::BinaryKernelOp>(op),
                               1000, 1e-6, 1e-6, 3u);
        NTT_PASS();
        NTT_TEST(BinaryF64_flat_ops);
        CheckFlatBinary<double>(static_cast<tensor::BinaryKernelOp>(op),
                                1000, 1e-12, 1e-12, 4u);
        NTT_PASS();
    }
    CheckDivIeeeEdges();
    CheckBinaryScalarArm();
    CheckClamp();
    CheckUnaryLog();
    CheckDriverStridedUnary();
    CheckDriverBroadcastSingleCell();
    std::cerr << "elementwise suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
