/*---
    main.cpp — test_native_tensor entry.

    NLANG_TENSOR_TIER (scalar|sse2|avx2) is set per ctest entry; the run
    asserts ActiveSimdTier() honors it, so every kernel suite below runs
    once per tier. A forced tier the host CPU cannot execute skips with
    exit 0 (recorded in the ctest output) instead of crashing on #UD.
---*/
#include "SimdCpu.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace test_native_tensor {
int RunVecMathTests();
//Later suites (matmul, elementwise, reductions) register here.
} // namespace test_native_tensor

int main() {
    const char* forced = std::getenv("NLANG_TENSOR_TIER");
    tensor::SimdTier expect = tensor::SimdTier::Scalar;
    if (forced != nullptr && std::strcmp(forced, "sse2") == 0) {
        expect = tensor::SimdTier::Sse2;
    } else if (forced != nullptr && std::strcmp(forced, "avx2") == 0) {
        expect = tensor::SimdTier::Avx2;
    } else if (forced != nullptr && std::strcmp(forced, "scalar") != 0) {
        //An invalid value must be ignored by the tier cache, not honored.
        forced = nullptr;
    }
    if (forced != nullptr && !tensor::CpuSupportsSimdTier(expect)) {
        std::cerr << "SKIP: host CPU lacks the forced tier " << forced << "\n";
        return 0;
    }
    if (forced != nullptr && tensor::ActiveSimdTier() != expect) {
        std::cerr << "tier forcing failed: env " << forced << "\n";
        return 2;
    }
    const int fails = test_native_tensor::RunVecMathTests();
    return fails == 0 ? 0 : 1;
}
