/*---
    tensor_random.cpp — whole-array random fill.

    uniform(dims, low, high): creates the dims-shaped array and fills it
    row-major with low + (high - low) * u, u in [0, 1) drawn from the
    host PRNG (the same seeded stream math.random draws; the VM owns
    the state). The draw is inherently sequential — the win here is
    killing the per-element fill loops, not lane math; a device
    backend slots a counter-based generator at this entry.
---*/
#include "TensorKernels.h"

#include "backend_registry.h"
#include <vector>



using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The host hands out raw 32-bit draws; the [0, 1) mapping takes the top
//24 bits (the same conversion math.random uses, so a seeded run draws
//the identical sequence through either surface).
constexpr unsigned kRngUnitShift = 8u;
constexpr double kRngUnitScale = 1.0 / 16777216.0;

double NextUnit(NativeHost* host) {
    return static_cast<double>(host->nextRandom(host) >> kRngUnitShift)
           * kRngUnitScale;
}

//uniform(dims, low, high): the dims vector shapes the creation (the
//full(dims, ...) creation path — shared rank-1 collector, negative-dim
//and element-count checks inside the creation), then one draw per element
//in row-major order. Computed in double, stored at the element width.
//low/high are read by the entry (the float cell packs into the low
//half, so the F arm reads a float, not a double) and widened here.
template <typename T>
void UniformKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   uint8_t kind, T lowArg, T highArg) {
    NativeArrayView dims;
    ArgArray(host, args, 0, &dims);
    std::vector<uint8_t> dimsStage;
    dims = StageOperandToHost(host, dims, &dimsStage);
    int32_t shape[nlang::kMaxArrayRank];
    const int32_t rank =
        CollectIntCells(host, dims, "tensor.uniform", shape);
    if (rank < 1)
        Raise(host, NEXC_Base, "tensor.uniform: dims length out of range");
    for (int32_t d = 0; d < rank; ++d) {
        if (shape[d] < 0)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.uniform: dims dim is negative");
    }
    const double low = static_cast<double>(lowArg);
    const double high = static_cast<double>(highArg);
    if (low > high)
        Raise(host, NEXC_Base,
              "tensor.uniform: low is greater than high");
    const int32_t handle = MintArray(host, kind, rank, shape);
    if (ActiveDeviceBackend().device) {
        //The device mint lands through a ForceHost staging twin: the
        //host PRNG fills the twin, one block H2D lands it (the P2
        //spelling — T3.7 replaces the fill with the MT19937 snapshot
        //kernel, same landing).
        const int32_t staging = NewArrayShaped(host, kind, rank, shape);
        NativeArrayView sv;
        host->getArrayView(host, staging, &sv);
        T* cells = CellsOf<T>(sv);
        const double span = high - low;
        for (int64_t i = 0; i < sv.elemCount; ++i)
            cells[i] = static_cast<T>(low + span * NextUnit(host));
        NativeArrayView dv;
        host->getArrayView(host, handle, &dv);
        CopyHostToDevice(host, dv.data, CellsOf<const T>(sv),
            static_cast<size_t>(sv.elemCount) * sv.elemCellBytes);
        ReturnInt(ret, handle);
        return;
    }
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    T* cells = CellsOf<T>(out);
    const double span = high - low;
    for (int64_t i = 0; i < out.elemCount; ++i)
        cells[i] = static_cast<T>(low + span * NextUnit(host));
    ReturnInt(ret, handle);
}

} //namespace

void TensorUniformF(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)argc;
    UniformKernel<float>(host, ret, args, NEK_Float32,
                         nlang::native::ArgFloat(args, 1),
                         nlang::native::ArgFloat(args, 2));
}

void TensorUniformD(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)argc;
    UniformKernel<double>(host, ret, args, NEK_Float64,
                          nlang::native::ArgDouble(args, 1),
                          nlang::native::ArgDouble(args, 2));
}

} //namespace tensor
