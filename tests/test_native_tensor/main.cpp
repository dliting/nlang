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

//Debug CRT: route assertion reports to stderr instead of the modal
//dialog, so ctest runs never block on a dialog.
#if defined(_CRTDBG_MAP_ALLOC) || defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace test_native_tensor {
int RunVecMathTests();
int RunMatMulTests();
int RunElementwiseTests();
int RunReduceTests();
int RunReduceStridedTests();
} // namespace test_native_tensor

int main(int argc, char** argv) {
#if defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    //Optional suite selection for bisection: native_tensor vecmath|
    //matmul|elementwise|reduce (default: all).
    const char* suite = argc > 1 ? argv[1] : nullptr;
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
    auto want = [&](const char* name) {
        return suite == nullptr || std::strcmp(suite, name) == 0;
    };
    int fails = 0;
    if (want("vecmath"))
        fails += test_native_tensor::RunVecMathTests();
    if (want("matmul"))
        fails += test_native_tensor::RunMatMulTests();
    if (want("elementwise"))
        fails += test_native_tensor::RunElementwiseTests();
    if (want("reduce"))
        fails += test_native_tensor::RunReduceTests();
    if (want("reducestrided"))
        fails += test_native_tensor::RunReduceStridedTests();
    return fails == 0 ? 0 : 1;
}
