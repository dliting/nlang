/*---
    tensor_activation.cpp — the ReLU gate backward and tanh of the
    tensor native module. P1 T1.2a migration: validation and output
    minting stay in the dispatcher; the run routes per call by the primary operand's residency
    (OpsForResidency) — reluGrad is the BinaryKernelOp::ReLUGrad binary (flat kernel
    on contiguous spans, paired odometers otherwise), tanh the VecMath
    unary lane. float/double only — integers have no activation
    semantics.
---*/
#include "TensorKernels.h"

#include "TensorOps.h"
#include "backend_registry.h"

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
    {
        const NativeArrayView operands[] = {vd, vz};
        AssertSameResidency(host, operands, 2);
    }
    if (vd.rank != vz.rank)
        Raise(host, NEXC_Base, "tensor.reluGrad: rank mismatch ("
            + ShapeText(vd) + " vs " + ShapeText(vz) + ")");
    for (int32_t d = 0; d < vd.rank; ++d) {
        if (vd.dims[d] != vz.dims[d])
            Raise(host, NEXC_Base, "tensor.reluGrad: shape mismatch ("
                + ShapeText(vd) + " vs " + ShapeText(vz) + ")");
    }
    const int32_t handle = MintOutputLike(host, vd.elemKind, vd.rank,
                                          vd.dims, vd);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(vd.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.binaryF32(host, BinaryKernelOp::ReLUGrad, vd, vz, out);
    } else {
        ops.binaryF64(host, BinaryKernelOp::ReLUGrad, vd, vz, out);
    }
    ReturnInt(ret, handle);
}

template <typename T>
void TanhKernel(NativeHost* host, uint8_t* ret, const uint8_t* args) {
    NativeArrayView v;
    ArgArray(host, args, 0, &v);
    const int32_t handle = MintOutputLike(host, v.elemKind, v.rank,
                                          v.dims, v);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    const TensorOps& ops = OpsForResidency(v.reserved[0]);
    if constexpr (std::is_same_v<T, float>) {
        ops.unaryF32(host, UnaryKernelOp::Tanh, v, out);
    } else {
        ops.unaryF64(host, UnaryKernelOp::Tanh, v, out);
    }
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
