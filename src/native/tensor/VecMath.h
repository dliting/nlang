/*---
    VecMath.h — public flat vector-math kernels of the tensor native
    module (the CPU analog of a device libdevice layer). Every entry
    forwards to the active KernelOps backend table (TensorBackends.h);
    contiguous flat spans in, results out, no shapes — the elementwise
    driver owns shapes/strides (see Elementwise.h). Interfaces pass raw
    pointers and counts only; vector types never cross the TU boundary.
---*/
#pragma once

#include <cstdint>

namespace tensor {

//e^x per lane. Contract: <=4 ulp relative error vs std::exp computed in
//double (the CPU backend's Cody-Waite reduction measures ~0.7 ulp);
//NaN propagates; +inf -> +inf; -inf -> +0; overflow -> +inf;
//underflow -> +0 (spec ruling D6). The f64 entry is the scalar lane
//today (rarely the bottleneck; a lane slot for a future polynomial).
void VecExpF32(const float* src, float* dst, int64_t count);
void VecExpF64(const double* src, double* dst, int64_t count);

//tanh per lane: 1e-6 relative error vs std::tanh, NaN propagates,
//saturates to +-1 at infinities. Two branches in the CPU backend: the
//exp form for large |x|, an odd Taylor under |x| = 0.5.
void VecTanhF32(const float* src, float* dst, int64_t count);
void VecTanhF64(const double* src, double* dst, int64_t count);

} // namespace tensor
