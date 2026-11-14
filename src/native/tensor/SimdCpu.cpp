/*---
    SimdCpu.cpp — runtime CPU feature detection (cpuid/xgetbv) and the
    cached NLANG_TENSOR_TIER override. AVX2 requires OS YMM state
    restoration (OSXSAVE + XCR0), not just the CPUID flag.
---*/
#include "SimdCpu.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#define NLANG_SIMD_HOST_X64 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace tensor {

namespace {

#if defined(NLANG_SIMD_HOST_X64)

#if defined(_MSC_VER)

void CpuId(int32_t regs[4], int32_t leaf, int32_t subleaf) {
    __cpuidex(regs, leaf, subleaf);
}

uint64_t Xcr0() {
    return _xgetbv(0);
}

#else

void CpuId(int32_t regs[4], int32_t leaf, int32_t subleaf) {
    uint32_t out[4];
    if (__get_cpuid_count(static_cast<uint32_t>(leaf),
                          static_cast<uint32_t>(subleaf),
                          &out[0], &out[1], &out[2], &out[3]) == 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
    }
    regs[0] = static_cast<int32_t>(out[0]);
    regs[1] = static_cast<int32_t>(out[1]);
    regs[2] = static_cast<int32_t>(out[2]);
    regs[3] = static_cast<int32_t>(out[3]);
}

uint64_t Xcr0() {
    uint32_t eax = 0;
    uint32_t edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}

#endif

//AVX2+FMA with OS YMM support: the full gate for the Avx2 tier.
bool DetectAvx2() {
    int32_t regs[4];
    CpuId(regs, 1, 0);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool fma = (regs[2] & (1 << 12)) != 0;
    if (!osxsave || !fma)
        return false;
    const uint64_t xcr0 = Xcr0();
    if ((xcr0 & 0x6) != 0x6) //XMM + YMM state both OS-saved
        return false;
    CpuId(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0; //AVX2
}

SimdTier DetectTier() {
    return DetectAvx2() ? SimdTier::Avx2 : SimdTier::Sse2;
}

#else //non-x64 host

SimdTier DetectTier() {
    return SimdTier::Scalar;
}

#endif //NLANG_SIMD_HOST_X64

} // namespace

SimdTier ActiveSimdTier() {
    static const SimdTier cached = [] {
        const char* forced = std::getenv("NLANG_TENSOR_TIER");
        if (forced != nullptr) {
            const bool avx2 = std::strcmp(forced, "avx2") == 0;
            const bool sse2 = std::strcmp(forced, "sse2") == 0;
            const bool scalar = std::strcmp(forced, "scalar") == 0;
            if (avx2 || sse2 || scalar) {
                const SimdTier picked = avx2 ? SimdTier::Avx2
                    : sse2 ? SimdTier::Sse2 : SimdTier::Scalar;
                //A forced tier the CPU cannot execute falls back to
                //detection — an avx2 name on a non-AVX2 host must not
                //end in #UD.
                return CpuSupportsSimdTier(picked) ? picked
                                                   : DetectTier();
            }
            //Anything else falls through to detection (test noise must
            //not select a tier).
        }
        return DetectTier();
    }();
    return cached;
}

bool CpuSupportsSimdTier(SimdTier tier) {
#if defined(NLANG_SIMD_HOST_X64)
    if (tier == SimdTier::Avx2)
        return DetectAvx2();
    if (tier == SimdTier::Sse2)
        return true; //architectural baseline of x64
    return true;
#else
    return tier == SimdTier::Scalar;
#endif
}

} // namespace tensor
