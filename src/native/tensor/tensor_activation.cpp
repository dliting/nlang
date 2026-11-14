/*---
    tensor_activation.cpp — the ReLU gate backward and tanh of the
    tensor native module. Both ride the tier-dispatched driver lanes:
    reluGrad is a strict same-shape binary (flat kernel on contiguous
    spans, carrier walk otherwise), tanh is the VecMath unary lane.
    float/double only — integers have no activation semantics.
---*/
#include "TensorKernels.h"

#include "Elementwise.h"

#include <cstdint>
#include <string>

using nlang::native::ArgArray;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//reluGrad(dOut, z) = z > 0 ? dOut : 0 — the ReLU backward gate (the
//gradient flows exactly where the pre-activation is positive; z = 0
//gates off, matching the forward clamp's subgradient at the kink).
template <typename T>
void ReluGradKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView vd;
    NativeArrayView vz;
    ArgArray(host, args, 0, &vd);
    ArgArray(host, args, 1, &vz);
    if (vd.elemKind != vz.elemKind)
        Raise(host, NEXC_Base, "tensor.reluGrad: element type mismatch");
    if (vd.rank != vz.rank)
        Raise(host, NEXC_Base, "tensor.reluGrad: rank mismatch ("
            + ShapeText(vd) + " vs " + ShapeText(vz) + ")");
    for (int32_t d = 0; d < vd.rank; ++d) {
        if (vd.dims[d] != vz.dims[d])
            Raise(host, NEXC_Base, "tensor.reluGrad: shape mismatch ("
                + ShapeText(vd) + " vs " + ShapeText(vz) + ")");
    }
    const int32_t handle = NewArrayShaped(host, vd.elemKind, vd.rank,
                                          vd.dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    if (!TryRunBinaryFlat<T>(vd, vz, out, BinaryKernelOp::ReLUGrad)) {
        //Same shape, non-contiguous: paired odometers — each view walks
        //its own recorded strides (a z slice view must not read d's).
        const T* dCells = CellsOf<const T>(vd);
        const T* zCells = CellsOf<const T>(vz);
        T* outCells = CellsOf<T>(out);
        int64_t idxD[nlang::kMaxArrayRank] = {0};
        int64_t idxZ[nlang::kMaxArrayRank] = {0};
        int64_t addrD = vd.offset;
        int64_t addrZ = vz.offset;
        for (int64_t i = 0; i < vd.elemCount; ++i) {
            outCells[i] = detail::CarrierBinary<T>(
                BinaryKernelOp::ReLUGrad, dCells[addrD], zCells[addrZ]);
            StrideWalkAdvance(vd, idxD, &addrD);
            StrideWalkAdvance(vz, idxZ, &addrZ);
        }
    }
    ReturnInt(ret, handle);
}

template <typename T>
void TanhKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t handle = NewArrayShaped(host, v.elemKind, v.rank,
                                          v.dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    RunUnaryFlat<T>(v, out, UnaryKernelOp::Tanh);
    ReturnInt(ret, handle);
}

} // namespace

void TensorReluGrad(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)argc;
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Float32:
            ReluGradKernel<float>(host, ret, args); break;
        case NEK_Float64:
            ReluGradKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base,
                  "tensor.reluGrad: element kind has no activation "
                  "semantics");
    }
}

void TensorTanh(NativeHost* host, uint8_t* ret, const uint8_t* args,
                int argc) {
    (void)argc;
    NativeArrayView v;
    v.elemKind = 0xEE;
    host->getArrayView(host, nlang::native::ArgInt(args, 0), &v);
    switch (v.elemKind) {
        case NEK_Float32:
            TanhKernel<float>(host, ret, args); break;
        case NEK_Float64:
            TanhKernel<double>(host, ret, args); break;
        default:
            Raise(host, NEXC_Base,
                  "tensor: element kind has no tanh semantics");
    }
}

} // namespace tensor
