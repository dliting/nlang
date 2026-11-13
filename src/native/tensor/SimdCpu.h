/*---
    SimdCpu.h — public tier interface of the tensor native module.
    No intrinsics here: this header is includable from any TU (the tier
    dispatch reads it inside the flat kernels; vector types never cross
    the TU boundary).
---*/
#pragma once

namespace tensor {

//Execution tiers of the SIMD kernels. Scalar is the portable reference;
//Sse2 is the x64 architectural baseline; Avx2+Fma is the performance
//tier (see the 2026-11-11 tensor vector ops spec, ruling D4).
enum class SimdTier {
    Scalar,
    Sse2,
    Avx2,
};

//Process-wide active tier, cached on first use. The NLANG_TENSOR_TIER
//environment variable (scalar|sse2|avx2) overrides the detection when
//it names a valid tier — a test-only hook; anything else falls back to
//the detected tier. Non-x64 builds are always Scalar.
SimdTier ActiveSimdTier();

//Whether the HOST CPU can actually execute a tier, independent of the
//override. Test entry points skip forced tiers the CPU lacks instead
//of crashing on #UD.
bool CpuSupportsSimdTier(SimdTier tier);

} // namespace tensor
