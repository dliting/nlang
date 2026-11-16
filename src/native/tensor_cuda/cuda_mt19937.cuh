/*---
    cuda_mt19937.cuh — the device-side MT19937 replica (P3 T3.7).

    Bit-identical to the VM's VmMt19937 (the C++ standard's algorithm):
    the uniform kernel receives the engine's 624-word state + block
    position via snapshotRngForDevice and regenerates the sequence from
    that point — the shared-stream contract (same seed → the device and
    the host draw the identical sequence, spec D4/8th-round N1).
---*/
#pragma once

#include <cstdint>

namespace tensor_cuda {

//The block transition over the whole 624-word state (the canonical
//two-loop form — the second loop reads the freshly generated head
//words, matching the sequence-index recurrence across the boundary).
__device__ inline void MtTwist(uint32_t* mt) {
    constexpr uint32_t kUpper = 0x80000000U;
    constexpr uint32_t kLower = 0x7fffffffU;
    constexpr uint32_t kMatrixA = 0x9908b0dfU;
    for (int32_t kk = 0; kk < 624 - 397; ++kk) {
        const uint32_t y = (mt[kk] & kUpper) | (mt[kk + 1] & kLower);
        mt[kk] = mt[kk + 397] ^ (y >> 1)
            ^ ((y & 1) != 0 ? kMatrixA : 0);
    }
    for (int32_t kk = 624 - 397; kk < 623; ++kk) {
        const uint32_t y = (mt[kk] & kUpper) | (mt[kk + 1] & kLower);
        mt[kk] = mt[kk + (397 - 624)] ^ (y >> 1)
            ^ ((y & 1) != 0 ? kMatrixA : 0);
    }
    const uint32_t y = (mt[623] & kUpper) | (mt[0] & kLower);
    mt[623] = mt[396] ^ (y >> 1) ^ ((y & 1) != 0 ? kMatrixA : 0);
}

//One tempered output from the state word (the standard tempering).
__device__ inline uint32_t MtTemper(uint32_t y) {
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= y >> 18;
    return y;
}

//The whole-block regeneration the uniform kernel uses: the twist runs
//once over the full state, then the words temper from the snapshot's
//position (the words before it were already tempered by the host).
__device__ inline void MtGenerateBlock(uint32_t* mt) {
    MtTwist(mt);
}

} // namespace tensor_cuda
