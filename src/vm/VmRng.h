/*---
    VmRng.h — the VM's PRNG engine: a self-implemented MT19937.

    Why not std::mt19937: the standard engine hides its state (no public
    accessor), and the device line's uniform kernel needs an exact
    snapshot — the 624-word state PLUS the block position (a
    math.random()-heavy run sits mid-block; a snapshot without the
    position would let the device re-temper from 0 and shift the shared
    sequence) — plus discard(n) to keep the host stream coherent after
    the device draws n values. Owning the state makes all three trivial.

    The algorithm implements the C++ standard's mersenne_twister_engine
    specification exactly (seed init, the 624-word transition with
    397/31, and the tempering), so sequences are bit-identical to
    std::mt19937 for the same seed — pinned by the vm_tests RNG test
    (pure draws, discard interleaving, multi-block skips).
---*/
#pragma once

#include <cstdint>

namespace nlang {

class VmMt19937 {
public:
    //Standard init_genrand: mt[0] = s; mt[i] = 1812433253 *
    //(mt[i-1] ^ (mt[i-1] >> 30)) + i.
    void seed(uint32_t s) {
        mt_[0] = s;
        for (uint32_t i = 1; i < 624; ++i) {
            mt_[i] = 1812433253U
                * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + i;
        }
        p_ = 624;   //exhausted: the first draw twists
    }

    //One tempered output (twists when the block is exhausted).
    uint32_t next() {
        if (p_ >= 624) {
            Twist();
            p_ = 0;
        }
        uint32_t y = mt_[p_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680U;
        y ^= (y << 15) & 0xefc60000U;
        y ^= y >> 18;
        return y;
    }

    //Advance n outputs without tempering (the standard's discard(n)).
    void discard(uint64_t n) {
        while (n > 0) {
            if (p_ >= 624) {
                Twist();
                p_ = 0;
            }
            const uint64_t left = 624 - p_;
            if (n >= left) {
                n -= left;
                p_ = 624;   //next draw twists
            } else {
                p_ += static_cast<uint32_t>(n);
                n = 0;
            }
        }
    }

    //The full engine state for a device snapshot (state + block
    //position — both are part of the standard's equality).
    void Snapshot(uint32_t out624[624], uint32_t* position) const {
        for (uint32_t i = 0; i < 624; ++i)
            out624[i] = mt_[i];
        *position = p_;
    }

    //Restore a snapshot (the device re-tempering must start exactly
    //where the host stream stands).
    void Restore(const uint32_t in624[624], uint32_t position) {
        for (uint32_t i = 0; i < 624; ++i)
            mt_[i] = in624[i];
        p_ = position;
    }

private:
    //The block transition. Written as the canonical two-loop form (the
    //first loop reads not-yet-rewritten tail words; the second reads
    //the freshly generated head words — the sequence-index recurrence
    //x[k+624] = x[k+397] ^ twist(x[k], x[k+1]) crosses the block
    //boundary there). y's upper bit from mt[kk], lower 31 from mt[kk+1].
    void Twist() {
        constexpr uint32_t kUpper = 0x80000000U;
        constexpr uint32_t kLower = 0x7fffffffU;
        constexpr uint32_t kMatrixA = 0x9908b0dfU;
        for (int32_t kk = 0; kk < 624 - 397; ++kk) {
            const uint32_t y = (mt_[kk] & kUpper)
                | (mt_[kk + 1] & kLower);
            mt_[kk] = mt_[kk + 397] ^ (y >> 1)
                ^ ((y & 1) != 0 ? kMatrixA : 0);
        }
        for (int32_t kk = 624 - 397; kk < 623; ++kk) {
            const uint32_t y = (mt_[kk] & kUpper)
                | (mt_[kk + 1] & kLower);
            mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1)
                ^ ((y & 1) != 0 ? kMatrixA : 0);
        }
        const uint32_t y = (mt_[623] & kUpper) | (mt_[0] & kLower);
        mt_[623] = mt_[396] ^ (y >> 1)
            ^ ((y & 1) != 0 ? kMatrixA : 0);
    }

    uint32_t mt_[624] = {};
    uint32_t p_ = 624;
};

} // namespace nlang
